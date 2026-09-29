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
    CHECK(outL[3] == 0.5f);
    CHECK(outL[4] == 0.0f && outR[15] == 0.0f);
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
    aab::StereoRing ring;
    ring.allocate(16);
    ring.setLatency(1, 100);

    std::vector<float> in(12), out(24);
    for (int round = 0; round < 5; ++round) {
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = float(round * 100 + int(i));
        CHECK(ring.writePlanar(in.data(), in.data(), 12) == 12);
        ring.readInterleaved(out.data(), 12);
        for (size_t i = 0; i < 12; ++i)
            CHECK(out[2 * i] == in[i] && out[2 * i + 1] == in[i]);
    }
    std::vector<float> big(20, 1.0f);
    CHECK(ring.writePlanar(big.data(), big.data(), 20) == 16);
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
    const int16_t pcm[4] = { 16384, -32768, 0, 32767 };
    ring.writeInterleavedInt16(pcm, 2);
    float out[4];
    ring.readInterleaved(out, 2);
    CHECK(out[0] == 0.5f && out[1] == -1.0f && out[2] == 0.0f);
    CHECK(std::fabs(out[3] - 1.0f) < 0.001f);
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
    while (last < float(total)) {
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
    testReset();
    testInt16Conversion();
    testConcurrentOrdering();
    if (failures == 0)
        std::printf("All ring buffer tests passed.\n");
    return failures == 0 ? 0 : 1;
}
