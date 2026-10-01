// Unit tests for StereoRing. Platform-independent; runs on Linux in CI.

#include "ring_buffer.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void testBuffersUntilTarget()
{
    aab::StereoRing ring;
    ring.allocate(1000);
    ring.setLatency(100, 200);

    std::vector<float> l(64, 1.0f), r(64, -1.0f), outL(64), outR(64);
    ring.writePlanar(l.data(), r.data(), 64);
    ring.readPlanar(outL.data(), outR.data(), 64);
    CHECK(outL[0] == 0.0f); // still buffering: 64 < 100
    CHECK(ring.queuedFrames() == 64);

    ring.writePlanar(l.data(), r.data(), 64);
    ring.readPlanar(outL.data(), outR.data(), 64);
    CHECK(outL[0] == 1.0f && outR[63] == -1.0f);
    CHECK(ring.queuedFrames() == 64);
}

static void testUnderrunPadsWithSilence()
{
    aab::StereoRing ring;
    ring.allocate(1000);
    ring.setLatency(10, 100);

    std::vector<float> in(20, 0.5f), outL(16), outR(16);
    ring.writePlanar(in.data(), in.data(), 20);
    ring.readPlanar(outL.data(), outR.data(), 16);
    CHECK(outL[15] == 0.5f && ring.underruns() == 0);
    ring.readPlanar(outL.data(), outR.data(), 16); // only 4 frames left
    // One frame stays queued for interpolation, so 3 come out.
    CHECK(outL[2] == 0.5f);
    CHECK(outL[3] == 0.0f && outR[15] == 0.0f);
    CHECK(ring.underruns() == 1);
}

static void testDiscardsBacklog()
{
    aab::StereoRing ring;
    ring.allocate(1000);
    ring.setLatency(50, 20);

    std::vector<float> in(400);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = float(i);
    ring.writePlanar(in.data(), in.data(), 400);

    std::vector<float> outL(10), outR(10);
    ring.readPlanar(outL.data(), outR.data(), 10);
    // 400 queued > 50 + 20, so it skips ahead to leave 50: first sample is 350.
    CHECK(outL[0] == 350.0f);
    CHECK(ring.discards() == 1);
    CHECK(ring.queuedFrames() == 40);
}

static void testWrapAroundAndFullRing()
{
    // A continuous ramp streamed through a small ring in uneven pieces must
    // come out as the same ramp: never going backwards, and following the
    // input closely once flowing.
    aab::StereoRing ring;
    ring.allocate(64);
    ring.setLatency(8, 100);

    float next = 1.0f, last = 0.0f;
    bool ordered = true, close = true;
    std::vector<float> in(12), out(26);
    for (int round = 0; round < 200; ++round) {
        for (auto& v : in)
            v = next++;
        CHECK(ring.writePlanar(in.data(), in.data(), 12) == 12);
        ring.readInterleaved(out.data(), 13 - round % 3);
        for (size_t i = 0; i < size_t(13 - round % 3); ++i) {
            const float v = out[2 * i];
            if (v == 0.0f)
                continue;
            if (v < last || v != out[2 * i + 1])
                ordered = false;
            if (v > next || v < next - 64.0f)
                close = false;
            last = v;
        }
    }
    CHECK(ordered);
    CHECK(close);
    CHECK(last > next - 64.0f);

    aab::StereoRing small;
    small.allocate(16);
    std::vector<float> big(20, 1.0f);
    CHECK(small.writePlanar(big.data(), big.data(), 20) == 16);
}

static void testFollowsClockDrift()
{
    // The producer delivers 0.1% more audio than the consumer reads in real
    // time. The consumer should speed up slightly to absorb it, with no
    // discards or underruns once settled, and keep the queue near its target.
    aab::StereoRing ring;
    ring.allocate(48000);
    ring.setLatency(1500, 2880);

    std::vector<float> in(481, 0.25f), l(480), r(480);
    for (int block = 0; block < 2; ++block)
        ring.writePlanar(in.data(), in.data(), 481);
    ring.readPlanar(l.data(), r.data(), 480);
    for (int block = 0; block < 4000; ++block) { // 40 seconds at 48 kHz
        ring.writePlanar(in.data(), in.data(), block % 2 ? 481 : 480);
        ring.readPlanar(l.data(), r.data(), 480);
        if (block == 1000)
            CHECK(ring.discards() == 0 && ring.underruns() == 0);
    }
    CHECK(ring.discards() == 0);
    CHECK(ring.underruns() == 0);
    CHECK(ring.ratio() > 1.0 && ring.ratio() <= 1.0 + aab::StereoRing::kMaxCorrection);
    CHECK(ring.queuedFrames() < 1500 + 2880);
    CHECK(l[479] == 0.25f);
}

static void testSlowsDownWhenStarved()
{
    // The producer delivers 0.1% less than real time: the consumer should
    // slow down slightly instead of running dry.
    aab::StereoRing ring;
    ring.allocate(48000);
    ring.setLatency(1500, 2880);

    std::vector<float> in(480, 0.25f), l(480), r(480);
    for (int block = 0; block < 4; ++block)
        ring.writePlanar(in.data(), in.data(), 480);
    for (int block = 0; block < 4000; ++block) {
        // 479.5 frames per block on average.
        ring.writePlanar(in.data(), in.data(), block % 2 ? 479 : 480);
        ring.readPlanar(l.data(), r.data(), 480);
    }
    CHECK(ring.underruns() == 0);
    CHECK(ring.discards() == 0);
    CHECK(ring.ratio() < 1.0);
}

static void testReset()
{
    aab::StereoRing ring;
    ring.allocate(100);
    ring.setLatency(5, 100);
    std::vector<float> in(10, 1.0f), out(20);
    ring.writePlanar(in.data(), in.data(), 10);
    ring.requestReset();
    ring.readInterleaved(out.data(), 10);
    CHECK(out[0] == 0.0f);
    CHECK(ring.queuedFrames() == 0);
}

static void testInt16Conversion()
{
    aab::StereoRing ring;
    ring.allocate(100);
    ring.setLatency(1, 100);
    const int16_t pcm[8] = { 16384, -32768, 0, 32767, 0, 32767, 0, 32767 };
    ring.writeInterleavedInt16(pcm, 4);
    float out[4];
    ring.readInterleaved(out, 2);
    CHECK(out[0] == 0.5f && out[1] == -1.0f);
    CHECK(std::fabs(out[2]) < 0.01f);
    CHECK(std::fabs(out[3] - 1.0f) < 0.01f);
}

static void testConcurrentOrdering()
{
    // Producer writes an increasing sequence; consumer must only ever see it
    // increasing (skips allowed, reordering or corruption not).
    aab::StereoRing ring;
    ring.allocate(512);
    ring.setLatency(64, 256);
    const int total = 200000;
    std::thread producer([&] {
        std::vector<float> chunk(32);
        int next = 1;
        while (next <= total) {
            for (auto& v : chunk)
                v = float(next <= total ? next : total), ++next;
            size_t done = 0;
            while (done < chunk.size())
                done += ring.writePlanar(chunk.data() + done, chunk.data() + done, chunk.size() - done);
        }
    });
    float last = 0.0f;
    bool ordered = true;
    std::vector<float> l(48), r(48);
    // The newest frame stays queued for interpolation, so stop one short.
    while (last < float(total - 1)) {
        ring.readPlanar(l.data(), r.data(), l.size());
        for (size_t i = 0; i < l.size(); ++i) {
            if (l[i] == 0.0f)
                continue;
            if (l[i] < last || l[i] != r[i])
                ordered = false;
            last = l[i];
        }
    }
    producer.join();
    CHECK(ordered);
}

int main()
{
    testBuffersUntilTarget();
    testUnderrunPadsWithSilence();
    testDiscardsBacklog();
    testWrapAroundAndFullRing();
    testFollowsClockDrift();
    testSlowsDownWhenStarved();
    testReset();
    testInt16Conversion();
    testConcurrentOrdering();
    if (failures == 0)
        std::printf("All ring buffer tests passed.\n");
    return failures == 0 ? 0 : 1;
}
