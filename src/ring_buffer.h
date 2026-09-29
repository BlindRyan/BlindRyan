#pragma once

// Lock-free single-producer / single-consumer ring of stereo float frames,
// stored interleaved (L R L R ...).
//
// The two ends run on different clocks (REAPER's audio interface on one side,
// a Windows audio stream on the other), so the consumer keeps the amount of
// queued audio near a target latency:
//   * it outputs silence until `target` frames have been queued,
//   * if the queue runs dry it outputs silence and waits for `target` again,
//   * if more than `target + slack` frames pile up it discards the excess.
// This trades an occasional tiny glitch for bounded, stable latency.

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
            [&](const float* src, size_t offset, size_t count) {
                for (size_t i = 0; i < count; ++i) {
                    left[offset + i] = src[2 * i] * gain;
                    right[offset + i] = src[2 * i + 1] * gain;
                }
            },
            [&](size_t offset, size_t count) {
                std::fill(left + offset, left + offset + count, 0.0f);
                std::fill(right + offset, right + offset + count, 0.0f);
            });
    }

    void readInterleaved(float* dst, size_t frames)
    {
        consume(frames,
            [&](const float* src, size_t offset, size_t count) {
                std::copy(src, src + 2 * count, dst + 2 * offset);
            },
            [&](size_t offset, size_t count) {
                std::fill(dst + 2 * offset, dst + 2 * (offset + count), 0.0f);
            });
    }

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

    template <class Copy, class Zero>
    void consume(size_t frames, Copy&& copy, Zero&& zero)
    {
        if (capacity_ == 0) {
            zero(0, frames);
            return;
        }
        const uint64_t w = write_.load(std::memory_order_acquire);
        uint64_t r = read_.load(std::memory_order_relaxed);

        if (resetRequested_.exchange(false)) {
            r = w;
            buffering_ = true;
        }

        size_t available = size_t(w - r);
        const size_t target = target_.load();

        if (buffering_) {
            if (available < std::max(target, frames)) {
                read_.store(r, std::memory_order_release);
                zero(0, frames);
                return;
            }
            buffering_ = false;
        }

        if (available > target + slack_.load()) {
            r += available - target;
            available = target;
            discards_.fetch_add(1);
        }

        const size_t n = std::min(frames, available);
        size_t done = 0;
        while (done < n) {
            const size_t pos = size_t((r + done) % capacity_);
            const size_t chunk = std::min(n - done, capacity_ - pos);
            copy(&buf_[2 * pos], done, chunk);
            done += chunk;
        }
        if (n < frames) {
            zero(n, frames - n);
            buffering_ = true;
            underruns_.fetch_add(1);
        }
        read_.store(r + n, std::memory_order_release);
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
    bool buffering_ = true; // consumer-only
};

} // namespace aab
