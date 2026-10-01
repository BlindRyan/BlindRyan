#include "announce.h"

// WIN32_LEAN_AND_MEAN leaves out the COM headers that the MSVC version of
// uiautomation.h relies on, so include them first.
#include <ole2.h>
#include <uiautomation.h>

namespace aab {

namespace {

using HostProviderFromHwndFn = HRESULT(WINAPI*)(HWND, IRawElementProviderSimple**);
using RaiseNotificationFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, NotificationKind,
    NotificationProcessing, BSTR, BSTR);

struct UiaFunctions {
    HostProviderFromHwndFn hostProviderFromHwnd = nullptr;
    RaiseNotificationFn raiseNotification = nullptr;

    UiaFunctions()
    {
        // Loaded at run time so the plugin still loads on systems where
        // UiaRaiseNotificationEvent is missing (older than Windows 10 1709).
        if (HMODULE uia = LoadLibraryW(L"uiautomationcore.dll")) {
            hostProviderFromHwnd = reinterpret_cast<HostProviderFromHwndFn>(
                reinterpret_cast<void*>(GetProcAddress(uia, "UiaHostProviderFromHwnd")));
            raiseNotification = reinterpret_cast<RaiseNotificationFn>(
                reinterpret_cast<void*>(GetProcAddress(uia, "UiaRaiseNotificationEvent")));
        }
    }
};

} // namespace

void announce(HWND hwnd, const std::wstring& text)
{
    static const UiaFunctions uia;
    if (!hwnd || text.empty() || !uia.hostProviderFromHwnd || !uia.raiseNotification)
        return;
    IRawElementProviderSimple* provider = nullptr;
    if (FAILED(uia.hostProviderFromHwnd(hwnd, &provider)) || !provider)
        return;
    BSTR message = SysAllocString(text.c_str());
    BSTR activity = SysAllocString(L"RhinoAudioRouter");
    uia.raiseNotification(provider, NotificationKind_ActionCompleted,
        NotificationProcessing_ImportantMostRecent, message, activity);
    SysFreeString(message);
    SysFreeString(activity);
    provider->Release();
}

} // namespace aab
