#include "engine.h"

#include "win_audio.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace aab {

namespace {

constexpr REFERENCE_TIME kBufferDuration = 200000; // 20 ms in 100 ns units

// Owns a COM interface pointer for the lifetime of a scope.
template <class T>
struct ComRef {
    T* p = nullptr;
    ~ComRef()
    {
        if (p)
            p->Release();
    }
    T* operator->() const { return p; }
};

struct HandleRef {
    HANDLE h = nullptr;
    ~HandleRef()
    {
        if (h)
            CloseHandle(h);
    }
};

WAVEFORMATEX makeFormat(double sampleRate, bool floatSamples)
{
    WAVEFORMATEX f {};
    f.wFormatTag = floatSamples ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = DWORD(std::lround(sampleRate));
    f.wBitsPerSample = floatSamples ? 32 : 16;
    f.nBlockAlign = WORD(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

} // namespace

Engine::Engine()
{
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    status_ = L"Not running yet. REAPER starts the plugin when audio is running.";
}

Engine::~Engine()
{
    stop();
    CloseHandle(wake_);
}

void Engine::start(double sampleRate, uint32_t maxBlockFrames)
{
    stop();
    sampleRate_ = sampleRate;
    maxBlock_ = std::max<uint32_t>(maxBlockFrames, 32);

    const size_t capacity = size_t(sampleRate) + size_t(maxBlock_) * 4;
    captureRing_.allocate(capacity);
    sendRing_.allocate(capacity);
    captureRing_.setLatency(maxBlock_ + size_t(sampleRate * 0.02), size_t(sampleRate * 0.06));

    quit_ = false;
    running_ = true;
    thread_ = std::thread([this] { threadMain(); });
}

void Engine::stop()
{
    if (!running_)
        return;
    quit_ = true;
    SetEvent(wake_);
    thread_.join();
    running_ = false;
    setStatus(L"Stopped. REAPER has paused audio for this plugin.");
}

void Engine::setTarget(const Target& target)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = target;
    }
    mode_.store(int(target.mode));
    captureRing_.requestReset();
    sendRing_.requestReset();
    targetVersion_.fetch_add(1);
    SetEvent(wake_);
}

Target Engine::target() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return target_;
}

std::wstring Engine::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Engine::setStatus(const std::wstring& text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (text == status_)
        return;
    status_ = text;
    statusVersion_.fetch_add(1);
}

bool Engine::waitForWake(DWORD ms)
{
    return WaitForSingleObject(wake_, ms) == WAIT_OBJECT_0 || quit_;
}

void Engine::notePeak(const float* left, const float* right, uint32_t frames, float gain)
{
    float block = 0.0f;
    for (uint32_t i = 0; i < frames; ++i)
        block = std::max(block, std::max(std::fabs(left[i]), std::fabs(right[i])));
    block *= std::fabs(gain);
    float current = peak_.load(std::memory_order_relaxed);
    while (block > current && !peak_.compare_exchange_weak(current, block)) {
    }
}

void Engine::readCapture(float* left, float* right, uint32_t frames, float gain)
{
    captureRing_.readPlanar(left, right, frames, gain);
    notePeak(left, right, frames, 1.0f);
}

void Engine::writeSend(const float* left, const float* right, uint32_t frames, float gain)
{
    sendRing_.writePlanar(left, right, frames, gain);
    notePeak(left, right, frames, gain);
}

void Engine::threadMain()
{
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (!quit_) {
        const Target t = target();
        if (t.mode == Mode::Capture) {
            if (t.program.empty()) {
                setStatus(L"No program chosen. Choose one in the Program list, then press Use selected.");
                waitForWake(INFINITE);
            } else {
                runCapture(t);
            }
        } else {
            if (t.deviceId.empty()) {
                setStatus(L"No output device chosen. Choose one in the Output device list, then press Use selected.");
                waitForWake(INFINITE);
            } else {
                runSend(t);
            }
        }
    }

    if (SUCCEEDED(com))
        CoUninitialize();
}

void Engine::runCapture(const Target& t)
{
    const std::wstring name = t.programLabel.empty() ? t.program : t.programLabel;

    const DWORD pid = findProgramProcess(t.program);
    if (pid == 0) {
        setStatus(L"Waiting for " + name + L" to start. Capture begins automatically when it runs.");
        waitForWake(2000);
        return;
    }

    ComRef<IAudioClient> client;
    HRESULT hr = activateProcessLoopback(pid, &client.p);
    if (FAILED(hr)) {
        setStatus(L"Could not capture " + name + L": " + describeHresult(hr) + L". Retrying.");
        waitForWake(3000);
        return;
    }

    // Ask Windows for float at REAPER's sample rate so no conversion is
    // needed here; fall back to 16-bit if that is refused.
    const DWORD baseFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    const DWORD convertFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    bool floatSamples = true;
    WAVEFORMATEX format = makeFormat(sampleRate_, true);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, baseFlags | convertFlags, kBufferDuration, 0, &format, nullptr);
    if (FAILED(hr)) {
        // Initialize can only be attempted once per client.
        client.p->Release();
        client.p = nullptr;
        floatSamples = false;
        format = makeFormat(sampleRate_, false);
        hr = activateProcessLoopback(pid, &client.p);
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, baseFlags | convertFlags, kBufferDuration, 0, &format, nullptr);
    }

    ComRef<IAudioCaptureClient> capture;
    HandleRef audioEvent { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    if (SUCCEEDED(hr))
        hr = client->SetEventHandle(audioEvent.h);
    if (SUCCEEDED(hr))
        hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture.p);
    if (SUCCEEDED(hr))
        hr = client->Start();
    if (FAILED(hr)) {
        setStatus(L"Could not start capturing " + name + L": " + describeHresult(hr) + L". Retrying.");
        waitForWake(3000);
        return;
    }

    HandleRef process { OpenProcess(SYNCHRONIZE, FALSE, pid) };
    captureRing_.requestReset();
    setStatus(L"Capturing " + name + L".");

    HANDLE waits[3] = { wake_, audioEvent.h, process.h };
    const DWORD waitCount = process.h ? 3 : 2;
    for (;;) {
        const DWORD w = WaitForMultipleObjects(waitCount, waits, FALSE, 1000);
        if (w == WAIT_OBJECT_0 || quit_)
            break; // target changed or stopping
        if (w == WAIT_OBJECT_0 + 2) {
            setStatus(name + L" closed. Waiting for it to start again.");
            waitForWake(1000);
            break;
        }
        // Drain every packet available (also on timeout, as a safety net).
        UINT32 packet = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr))
                break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                captureRing_.writeSilence(frames);
            else if (floatSamples)
                captureRing_.writeInterleaved(reinterpret_cast<const float*>(data), frames);
            else
                captureRing_.writeInterleavedInt16(reinterpret_cast<const int16_t*>(data), frames);
            capture->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            setStatus(L"Capture of " + name + L" stopped: " + describeHresult(hr) + L". Reconnecting.");
            waitForWake(1000);
            break;
        }
    }
    client->Stop();
}

void Engine::runSend(const Target& t)
{
    const std::wstring name = t.deviceLabel.empty() ? t.deviceId : t.deviceLabel;

    ComRef<IMMDeviceEnumerator> enumerator;
    ComRef<IMMDevice> device;
    ComRef<IAudioClient> client;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator.p);
    if (SUCCEEDED(hr))
        hr = enumerator->GetDevice(t.deviceId.c_str(), &device.p);
    if (FAILED(hr)) {
        setStatus(L"Waiting for " + name + L" to become available.");
        waitForWake(2000);
        return;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client.p);

    WAVEFORMATEX format = makeFormat(sampleRate_, true);
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            kBufferDuration, 0, &format, nullptr);

    ComRef<IAudioRenderClient> render;
    HandleRef audioEvent { CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    UINT32 bufferFrames = 0;
    if (SUCCEEDED(hr))
        hr = client->SetEventHandle(audioEvent.h);
    if (SUCCEEDED(hr))
        hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr))
        hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render.p);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = render->GetBuffer(bufferFrames, &data)))
        hr = render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (SUCCEEDED(hr))
        hr = client->Start();
    if (FAILED(hr)) {
        setStatus(L"Could not send to " + name + L": " + describeHresult(hr) + L". Retrying.");
        waitForWake(3000);
        return;
    }

    sendRing_.setLatency(maxBlock_ + bufferFrames, size_t(sampleRate_ * 0.06));
    sendRing_.requestReset();
    setStatus(L"Sending this track to " + name + L".");

    HANDLE waits[2] = { wake_, audioEvent.h };
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 1000);
        if (w == WAIT_OBJECT_0 || quit_)
            break;
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr) && padding < bufferFrames) {
            const UINT32 frames = bufferFrames - padding;
            hr = render->GetBuffer(frames, &data);
            if (SUCCEEDED(hr)) {
                sendRing_.readInterleaved(reinterpret_cast<float*>(data), frames);
                hr = render->ReleaseBuffer(frames, 0);
            }
        }
        if (FAILED(hr)) {
            setStatus(L"Sending to " + name + L" stopped: " + describeHresult(hr) + L". Reconnecting.");
            waitForWake(1000);
            break;
        }
    }
    client->Stop();
}

} // namespace aab
