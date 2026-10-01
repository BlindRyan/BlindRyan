#pragma once

// Lock-free single-producer / single-consumer ring of stereo float frames,
// stored interleaved (L R L R ...).
//
// The two ends run on different clocks (the host's audio interface on one
// side, a Windows audio stream on the other), so the consumer keeps the amount
// of queued audio near a target latency:
//   * it outputs silence until `target` frames have been queued,
//   * it reads slightly faster or slower than real time (at most 0.2%, too
//     little to hear) to follow the drift between the two clocks smoothly,
//     using cubic interpolation between frames,
//   * only if that cannot keep up (the queue runs dry, or more than
//     `target + slack` frames pile up) does it fall back to outputting
//     silence or discarding the excess.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aab {

class StereoRing {
public:
    // Not thread-safe: call before either end starts using the ring.
    void allocate(size_t capacityFrames)
    {
        buf_.assign(capacityFrames * 2, 0.0f);
        capacity_ = capacityFrames;
        write_.store(0);
        read_.store(0);
        buffering_ = true;
        resetRequested_.store(false);
    }

    // Any thread. Applied by the consumer the next time it reads.
    void setLatency(size_t targetFrames, size_t slackFrames)
    {
        target_.store(std::min(targetFrames, capacity_ / 2));
        slack_.store(slackFrames);
    }

    // Any thread. The consumer drops everything queued and re-buffers.
    void requestReset() { resetRequested_.store(true); }

    size_t queuedFrames() const { return size_t(write_.load() - read_.load()); }
    uint32_t underruns() const { return underruns_.load(); }
    uint32_t discards() const { return discards_.load(); }

    // ---- Producer side ----

    // Writes planar input. `right` may equal `left` for mono.
    size_t writePlanar(const float* left, const float* right, size_t frames, float gain = 1.0f)
    {
        return produce(frames, [&](float* dst, size_t offset, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                dst[2 * i] = left[offset + i] * gain;
                dst[2 * i + 1] = right[offset + i] * gain;
            }
        });
    }

    size_t writeInterleaved(const float* src, size_t frames)
    {
        return produce(frames, [&](float* dst, size_t offset, size_t count) {
            std::copy(src + 2 * offset, src + 2 * (offset + count), dst);
        });
    }

    size_t writeInterleavedInt16(const int16_t* src, size_t frames)
    {
        return produce(frames, [&](float* dst, size_t offset, size_t count) {
            for (size_t i = 0; i < 2 * count; ++i)
                dst[i] = float(src[2 * offset + i]) * (1.0f / 32768.0f);
        });
    }

    size_t writeSilence(size_t frames)
    {
        return produce(frames, [](float* dst, size_t, size_t count) {
            std::fill(dst, dst + 2 * count, 0.0f);
        });
    }

    // ---- Consumer side (always fills exactly `frames`, padding with silence) ----

    void readPlanar(float* left, float* right, size_t frames, float gain = 1.0f)
    {
        consume(frames,
            [&](size_t i, float l, float r) {
                left[i] = l * gain;
                right[i] = r * gain;
            });
    }

    void readInterleaved(float* dst, size_t frames)
    {
        consume(frames, [&](size_t i, float l, float r) {
            dst[2 * i] = l;
            dst[2 * i + 1] = r;
        });
    }

    // Current read speed relative to real time (1.0 = no correction).
    double ratio() const { return ratio_.load(); }

    // Largest speed correction, as a fraction of real time.
    static constexpr double kMaxCorrection = 0.002;

private:
    template <class Fill>
    size_t produce(size_t frames, Fill&& fill)
    {
        if (capacity_ == 0)
            return 0;
        const uint64_t w = write_.load(std::memory_order_relaxed);
        const uint64_t r = read_.load(std::memory_order_acquire);
        const size_t space = capacity_ - size_t(w - r);
        const size_t n = std::min(frames, space);
        size_t done = 0;
        while (done < n) {
            const size_t pos = size_t((w + done) % capacity_);
            const size_t chunk = std::min(n - done, capacity_ - pos);
            fill(&buf_[2 * pos], done, chunk);
            done += chunk;
        }
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    const float* frameAt(uint64_t index) const { return &buf_[2 * size_t(index % capacity_)]; }

    // Restart interpolation cleanly at frame `r` (after a reset or a jump).
    void restartAt(uint64_t r)
    {
        frac_ = 0.0;
        const float* f = frameAt(r);
        prev_[0] = f[0];
        prev_[1] = f[1];
    }

    static float cubic(float xm1, float x0, float x1, float x2, float t)
    {
        // Catmull-Rom spline through four neighbouring samples.
        return x0
            + 0.5f * t * (x1 - xm1 + t * (2.0f * xm1 - 5.0f * x0 + 4.0f * x1 - x2 + t * (3.0f * (x0 - x1) + x2 - xm1)));
    }

    template <class Put>
    void consume(size_t frames, Put&& put)
    {
        size_t i = 0;
        if (capacity_ != 0) {
            const uint64_t w = write_.load(std::memory_order_acquire);
            uint64_t r = read_.load(std::memory_order_relaxed);

            if (resetRequested_.exchange(false)) {
                r = w;
                buffering_ = true;
            }

            size_t available = size_t(w - r);
            const size_t target = std::max<size_t>(target_.load(), 1);

            if (buffering_) {
                if (available < std::max(target, frames + 2)) {
                    read_.store(r, std::memory_order_release);
                    for (; i < frames; ++i)
                        put(i, 0.0f, 0.0f);
                    return;
                }
                buffering_ = false;
                restartAt(r);
                smoothedFill_ = double(available);
            }

            if (available > target + slack_.load()) {
                r += available - target;
                available = target;
                discards_.fetch_add(1);
                restartAt(r);
                smoothedFill_ = double(available);
            }

            // Follow the queue level slowly (time constant of roughly 16k
            // frames) and nudge the read speed towards the target level.
            const double alpha = std::min(1.0, double(frames) / 16384.0);
            smoothedFill_ += alpha * (double(available) - smoothedFill_);
            const double error = (smoothedFill_ - double(target)) / double(target);
            const double ratio = 1.0 + std::clamp(error * 2.0 * kMaxCorrection, -kMaxCorrection, kMaxCorrection);
            ratio_.store(ratio, std::memory_order_relaxed);

            // Each output needs the frame after the read position, so one
            // frame always stays queued until the next frame arrives.
            for (; i < frames && r + 1 < w; ++i) {
                const float* x0 = frameAt(r);
                const float* x1 = frameAt(r + 1);
                const float* x2 = r + 2 < w ? frameAt(r + 2) : x1;
                const float t = float(frac_);
                put(i, cubic(prev_[0], x0[0], x1[0], x2[0], t), cubic(prev_[1], x0[1], x1[1], x2[1], t));
                frac_ += ratio;
                while (frac_ >= 1.0 && r + 1 < w) {
                    prev_[0] = frameAt(r)[0];
                    prev_[1] = frameAt(r)[1];
                    frac_ -= 1.0;
                    ++r;
                }
            }
            if (i < frames) {
                buffering_ = true;
                underruns_.fetch_add(1);
            }
            read_.store(r, std::memory_order_release);
        }
        for (; i < frames; ++i)
            put(i, 0.0f, 0.0f);
    }

    std::vector<float> buf_;
    size_t capacity_ = 0;
    std::atomic<uint64_t> write_ { 0 };
    std::atomic<uint64_t> read_ { 0 };
    std::atomic<size_t> target_ { 0 };
    std::atomic<size_t> slack_ { 0 };
    std::atomic<bool> resetRequested_ { false };
    std::atomic<uint32_t> underruns_ { 0 };
    std::atomic<uint32_t> discards_ { 0 };
    std::atomic<double> ratio_ { 1.0 };
    // Consumer-only state.
    bool buffering_ = true;
    double frac_ = 0.0;      // position between frame read_ and the next
    float prev_[2] = {};     // the frame before read_, for interpolation
    double smoothedFill_ = 0.0;
};

} // namespace aab
