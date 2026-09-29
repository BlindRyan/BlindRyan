#include "win_audio.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <map>
#include <set>
#include <thread>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// MinGW-w64 does not ship this Windows 10 SDK header yet.
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

namespace aab {

namespace {

// PKEY_Device_FriendlyName, defined here so no GUID library is needed.
const PROPERTYKEY kDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { reset(); }
    void reset()
    {
        if (p)
            p->Release();
        p = nullptr;
    }
    T** put()
    {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring lower(std::wstring s)
{
    for (auto& c : s)
        c = wchar_t(std::towlower(c));
    return s;
}

struct ProcessEntry {
    DWORD parent = 0;
    std::wstring exe;
};

std::map<DWORD, ProcessEntry> snapshotProcesses()
{
    std::map<DWORD, ProcessEntry> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W pe {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        result[pe.th32ProcessID] = { pe.th32ParentProcessID, pe.szExeFile };
    CloseHandle(snap);
    return result;
}

// Browsers and many apps play sound from a child process with the same exe
// name. Walk up to the topmost such ancestor so capturing its tree gets all
// of the program's audio.
DWORD rootOf(DWORD pid, const std::map<DWORD, ProcessEntry>& procs)
{
    std::set<DWORD> seen;
    for (;;) {
        seen.insert(pid);
        auto it = procs.find(pid);
        if (it == procs.end())
            return pid;
        auto parent = procs.find(it->second.parent);
        if (parent == procs.end() || seen.count(it->second.parent)
            || lower(parent->second.exe) != lower(it->second.exe))
            return pid;
        pid = it->second.parent;
    }
}

std::wstring fileDescription(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t path[MAX_PATH * 2];
    DWORD len = DWORD(std::size(path));
    BOOL ok = QueryFullProcessImageNameW(h, 0, path, &len);
    CloseHandle(h);
    if (!ok)
        return {};

    DWORD unused = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &unused);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data()))
        return {};
    struct Translation {
        WORD language, codepage;
    }* tr = nullptr;
    UINT trLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) || trLen < sizeof(Translation))
        return {};
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->language, tr->codepage);
    wchar_t* desc = nullptr;
    UINT descLen = 0;
    if (!VerQueryValueW(data.data(), key, (void**)&desc, &descLen) || descLen == 0)
        return {};
    std::wstring s(desc);
    while (!s.empty() && std::iswspace(s.back()))
        s.pop_back();
    return s;
}

std::wstring makeLabel(DWORD pid, const std::wstring& exe)
{
    std::wstring desc = fileDescription(pid);
    if (desc.empty() || lower(desc) == lower(exe))
        return exe;
    return desc + L" (" + exe + L")";
}

// PIDs of every audio session on every active output device.
std::set<DWORD> audioSessionPids()
{
    std::set<DWORD> pids;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)enumerator.put())))
        return pids;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put())))
        return pids;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(i, device.put()))
            || FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)manager.put()))
            || FAILED(manager->GetSessionEnumerator(sessions.put())))
            continue;
        int n = 0;
        sessions->GetCount(&n);
        for (int j = 0; j < n; ++j) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(sessions->GetSession(j, control.put()))
                || FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), (void**)control2.put())))
                continue;
            if (control2->IsSystemSoundsSession() == S_OK)
                continue;
            DWORD pid = 0;
            if (SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0)
                pids.insert(pid);
        }
    }
    return pids;
}

BOOL CALLBACK collectWindowPid(HWND hwnd, LPARAM param)
{
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || GetWindowTextLengthW(hwnd) == 0
        || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    reinterpret_cast<std::set<DWORD>*>(param)->insert(pid);
    return TRUE;
}

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivateHandler() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    virtual ~ActivateHandler() { CloseHandle(done_); }
    HANDLE doneEvent() const { return done_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
    {
        SetEvent(done_);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    HANDLE done_;
};

} // namespace

void runInMta(const std::function<void()>& fn)
{
    std::thread worker([&] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        fn();
        if (SUCCEEDED(hr))
            CoUninitialize();
    });
    worker.join();
}

std::vector<ProgramInfo> listPrograms(bool includeWindowed)
{
    const auto procs = snapshotProcesses();
    const DWORD self = GetCurrentProcessId();

    std::set<DWORD> windowed;
    if (includeWindowed)
        EnumWindows(collectWindowPid, reinterpret_cast<LPARAM>(&windowed));

    std::map<std::wstring, ProgramInfo> byExe; // one entry per program
    auto add = [&](DWORD pid, bool hasAudio) {
        if (pid == self || pid == 0)
            return;
        DWORD root = rootOf(pid, procs);
        auto it = procs.find(root);
        if (root == self || it == procs.end())
            return;
        std::wstring key = lower(it->second.exe);
        auto& info = byExe[key];
        if (info.pid == 0) {
            info.pid = root;
            info.exe = it->second.exe;
        }
        info.hasAudio = info.hasAudio || hasAudio;
    };
    for (DWORD pid : audioSessionPids())
        add(pid, true);
    for (DWORD pid : windowed)
        add(pid, false);

    std::vector<ProgramInfo> list;
    for (auto& [key, info] : byExe) {
        info.label = makeLabel(info.pid, info.exe);
        list.push_back(info);
    }
    std::sort(list.begin(), list.end(), [](const ProgramInfo& a, const ProgramInfo& b) {
        if (a.hasAudio != b.hasAudio)
            return a.hasAudio;
        return lower(a.label) < lower(b.label);
    });
    return list;
}

std::vector<DeviceInfo> listOutputDevices()
{
    std::vector<DeviceInfo> list;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)enumerator.put()))
        || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put())))
        return list;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        LPWSTR id = nullptr;
        if (FAILED(devices->Item(i, device.put())) || FAILED(device->GetId(&id)))
            continue;
        DeviceInfo info;
        info.id = id;
        CoTaskMemFree(id);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &name)) && name.vt == VT_LPWSTR)
                info.name = name.pwszVal;
            PropVariantClear(&name);
        }
        if (info.name.empty())
            info.name = info.id;
        list.push_back(std::move(info));
    }
    std::sort(list.begin(), list.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        return lower(a.name) < lower(b.name);
    });
    return list;
}

DWORD findProgramProcess(const std::wstring& exe)
{
    const auto procs = snapshotProcesses();
    const std::wstring want = lower(exe);
    const DWORD self = GetCurrentProcessId();
    auto matches = [&](DWORD pid) {
        auto it = procs.find(pid);
        return pid != self && it != procs.end() && lower(it->second.exe) == want;
    };
    for (DWORD pid : audioSessionPids()) {
        if (matches(pid))
            return rootOf(pid, procs);
    }
    for (const auto& [pid, entry] : procs) {
        if (matches(pid))
            return rootOf(pid, procs);
    }
    return 0;
}

HRESULT activateProcessLoopback(DWORD pid, IAudioClient** client)
{
    *client = nullptr;
    AUDIOCLIENT_ACTIVATION_PARAMS params {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams;
    PropVariantInit(&activateParams);
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient), &activateParams, handler, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->doneEvent(), 5000) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            HRESULT activateResult = E_FAIL;
            IUnknown* unknown = nullptr;
            hr = op->GetActivateResult(&activateResult, &unknown);
            if (SUCCEEDED(hr))
                hr = activateResult;
            if (SUCCEEDED(hr) && unknown)
                hr = unknown->QueryInterface(__uuidof(IAudioClient), (void**)client);
            if (unknown)
                unknown->Release();
        }
    }
    if (op)
        op->Release();
    handler->Release();
    return hr;
}

std::wstring describeHresult(HRESULT hr)
{
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return L"the audio device was removed or disabled";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return L"the device does not accept this audio format";
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"the device is in use by another program in exclusive mode";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return L"the Windows Audio service is not running";
    case E_ACCESSDENIED:
        return L"Windows denied access";
    case E_NOTIMPL:
        return L"this version of Windows does not support per-program capture";
    }
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(hr), 0,
        buf, DWORD(std::size(buf)), nullptr);
    std::wstring text(buf);
    while (!text.empty() && std::iswspace(text.back()))
        text.pop_back();
    wchar_t code[32];
    swprintf(code, 32, L"error 0x%08lX", static_cast<unsigned long>(hr));
    return text.empty() ? std::wstring(code) : text + L" (" + code + L")";
}

} // namespace aab
