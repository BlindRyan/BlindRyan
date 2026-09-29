#pragma once

// Windows helpers: listing programs and audio devices, finding the process
// to capture, and activating a WASAPI process-loopback client.

#include <windows.h>

#include <audioclient.h>

#include <functional>
#include <string>
#include <vector>

namespace aab {

struct ProgramInfo {
    DWORD pid = 0;          // root process of the program's process tree
    std::wstring exe;       // e.g. "chrome.exe"; what we save in the project
    std::wstring label;     // e.g. "Google Chrome (chrome.exe)"
    bool hasAudio = false;  // has an audio session, so it has played sound
};

struct DeviceInfo {
    std::wstring id;    // endpoint ID string; what we save in the project
    std::wstring name;  // e.g. "CABLE Input (VB-Audio Virtual Cable)"
};

// Runs `fn` on a temporary thread initialised for COM (multithreaded
// apartment) and waits for it. Lets the GUI thread use WASAPI without caring
// how the host initialised COM on its own thread.
void runInMta(const std::function<void()>& fn);

// The calls below need COM initialised on the calling thread.

// Programs that have an audio session on any active output device, and, if
// `includeWindowed`, every program with a visible titled window. Excludes
// this process (capturing REAPER into itself would feed back).
std::vector<ProgramInfo> listPrograms(bool includeWindowed);

std::vector<DeviceInfo> listOutputDevices();

// Finds the process to capture for a program saved as `exe`. Prefers one that
// has an audio session. Returns 0 if the program is not running.
DWORD findProgramProcess(const std::wstring& exe);

// Creates an audio client that captures `pid` and all its child processes.
HRESULT activateProcessLoopback(DWORD pid, IAudioClient** client);

std::wstring describeHresult(HRESULT hr);

} // namespace aab
