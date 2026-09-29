#pragma once

// The audio engine: a background thread that either captures one program's
// sound (Capture mode) or plays the track's sound to an output device (Send
// mode), connected to REAPER's audio thread through lock-free rings.
//
// Each ring always has the same producer and consumer thread, whatever the
// mode, so switching modes never races:
//   captureRing_: worker thread writes, audio thread reads.
//   sendRing_:    audio thread writes, worker thread reads.

#include "ring_buffer.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace aab {

enum class Mode : int { Capture = 0, Send = 1 };

struct Target {
    Mode mode = Mode::Capture;
    std::wstring program;      // exe name, e.g. "chrome.exe"
    std::wstring programLabel; // friendly name for messages
    std::wstring deviceId;
    std::wstring deviceLabel;
};

class Engine {
public:
    Engine();
    ~Engine();

    // Main thread.
    void start(double sampleRate, uint32_t maxBlockFrames);
    void stop();
    bool running() const { return running_; }

    // Any non-audio thread.
    void setTarget(const Target& target);
    Target target() const;
    uint32_t targetVersion() const { return targetVersion_.load(); }
    std::wstring status() const;
    uint32_t statusVersion() const { return statusVersion_.load(); }
    // Highest level since the last call, as linear amplitude.
    float takePeak() { return peak_.exchange(0.0f); }

    // Audio thread.
    Mode mode() const { return Mode(mode_.load(std::memory_order_relaxed)); }
    void readCapture(float* left, float* right, uint32_t frames, float gain);
    void writeSend(const float* left, const float* right, uint32_t frames, float gain);

private:
    void threadMain();
    void runCapture(const Target& target);
    void runSend(const Target& target);
    bool waitForWake(DWORD ms); // true if woken by a target change or stop
    void setStatus(const std::wstring& text);
    void notePeak(const float* left, const float* right, uint32_t frames, float gain);

    StereoRing captureRing_;
    StereoRing sendRing_;

    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> quit_ { false };
    bool running_ = false;
    double sampleRate_ = 48000.0;
    uint32_t maxBlock_ = 1024;

    mutable std::mutex mutex_;
    Target target_;
    std::wstring status_;
    std::atomic<uint32_t> targetVersion_ { 0 };
    std::atomic<uint32_t> statusVersion_ { 0 };
    std::atomic<int> mode_ { 0 };
    std::atomic<float> peak_ { 0.0f };
};

} // namespace aab
