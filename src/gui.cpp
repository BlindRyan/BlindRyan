#include "gui.h"

#include "announce.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>

namespace aab {

namespace {

constexpr wchar_t kWindowClass[] = L"AppAudioBridgeWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 500;
constexpr int kWidth = 504;  // logical pixels at 96 DPI
constexpr int kHeight = 404;

enum ControlId {
    IdModeCombo = 100,
    IdProgramList,
    IdDeviceList,
    IdShowAll,
    IdUse,
    IdStop,
    IdRefresh,
    IdLevel,
    IdStatus,
};

bool classRegistered = false;

HINSTANCE moduleHandle()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleHandle), &module);
    return module;
}

bool sameExe(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

bool isButton(int id)
{
    return id == IdShowAll || id == IdUse || id == IdStop || id == IdRefresh || id == IdLevel;
}

} // namespace

Gui::Gui(Engine& engine, std::function<void()> onTargetChanged)
    : engine_(engine)
    , onTargetChanged_(std::move(onTargetChanged))
{
}

Gui::~Gui()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
    if (font_)
        DeleteObject(font_);
}

void Gui::unregisterWindowClass()
{
    if (classRegistered)
        UnregisterClassW(kWindowClass, moduleHandle());
    classRegistered = false;
}

int Gui::px(int logical) const
{
    const double scale = scale_ > 0.0 ? scale_ : GetDpiForSystem() / 96.0;
    return int(std::lround(logical * scale));
}

void Gui::getSize(uint32_t* width, uint32_t* height) const
{
    *width = uint32_t(px(kWidth));
    *height = uint32_t(px(kHeight));
}

bool Gui::attach(HWND parent)
{
    if (!classRegistered) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = windowProc;
        wc.hInstance = moduleHandle();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc))
            return false;
        classRegistered = true;
    }
    // WS_EX_CONTROLPARENT lets REAPER's own dialog keyboard handling Tab
    // into our controls as if they were part of the FX window.
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, L"App Audio Bridge",
        WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE, 0, 0, px(kWidth), px(kHeight),
        parent, nullptr, moduleHandle(), this);
    if (!hwnd_)
        return false;
    createControls();
    refreshLists(false);
    seenTargetVersion_ = engine_.targetVersion();
    seenStatusVersion_ = engine_.statusVersion();
    SetWindowTextW(statusEdit_, engine_.status().c_str());
    SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
    return true;
}

void Gui::show(bool visible)
{
    if (hwnd_)
        ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void Gui::createControls()
{
    NONCLIENTMETRICSW metrics {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);

    HINSTANCE inst = moduleHandle();
    // Creation order is the Tab order. Each list or box comes right after its
    // label, which is how screen readers find the control's name.
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                    int x, int y, int w, int h, DWORD exStyle = 0) {
        HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
            px(x), px(y), px(w), px(h), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        if (id != 0)
            SetWindowSubclass(c, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
        return c;
    };

    const int m = 12, w = kWidth - 2 * m;
    make(L"STATIC", L"&Mode:", SS_LEFT, 0, m, 12, w, 18);
    modeCombo_ = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdModeCombo, m, 32, w, 200);
    SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Capture audio from a program"));
    SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Send this track to an output device"));

    // The program and device lists share one spot; only the one for the
    // current mode is shown, so the hidden one is skipped by Tab.
    programLabel_ = make(L"STATIC", L"&Program to capture:", SS_LEFT, 0, m, 66, w, 18);
    programList_ = make(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
        IdProgramList, m, 86, w, 150, WS_EX_CLIENTEDGE);
    deviceLabel_ = make(L"STATIC", L"Output &device to send to:", SS_LEFT, 0, m, 66, w, 18);
    deviceList_ = make(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
        IdDeviceList, m, 86, w, 150, WS_EX_CLIENTEDGE);
    showAllCheck_ = make(L"BUTTON", L"Show &all programs, not only ones that have played sound",
        BS_AUTOCHECKBOX | WS_TABSTOP, IdShowAll, m, 242, w, 22);

    make(L"BUTTON", L"&Use selected", BS_PUSHBUTTON | WS_TABSTOP, IdUse, m, 272, 110, 28);
    make(L"BUTTON", L"S&top", BS_PUSHBUTTON | WS_TABSTOP, IdStop, m + 116, 272, 80, 28);
    make(L"BUTTON", L"&Refresh lists", BS_PUSHBUTTON | WS_TABSTOP, IdRefresh, m + 202, 272, 110, 28);
    make(L"BUTTON", L"Announce &level", BS_PUSHBUTTON | WS_TABSTOP, IdLevel, m + 318, 272, 130, 28);

    make(L"STATIC", L"&Status:", SS_LEFT, 0, m, 312, w, 18);
    statusEdit_ = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
        IdStatus, m, 332, w, 60, WS_EX_CLIENTEDGE);

    SendMessageW(modeCombo_, CB_SETCURSEL, WPARAM(engine_.target().mode), 0);
    updateModeVisibility();
}

Mode Gui::selectedMode() const
{
    return SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0) == 1 ? Mode::Send : Mode::Capture;
}

void Gui::updateModeVisibility()
{
    const bool capture = selectedMode() == Mode::Capture;
    ShowWindow(programLabel_, capture ? SW_SHOW : SW_HIDE);
    ShowWindow(programList_, capture ? SW_SHOW : SW_HIDE);
    ShowWindow(showAllCheck_, capture ? SW_SHOW : SW_HIDE);
    ShowWindow(deviceLabel_, capture ? SW_HIDE : SW_SHOW);
    ShowWindow(deviceList_, capture ? SW_HIDE : SW_SHOW);
}

void Gui::refreshLists(bool announceCount)
{
    const bool showAll = SendMessageW(showAllCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    runInMta([&] {
        programs_ = listPrograms(showAll);
        devices_ = listOutputDevices();
    });

    // Keep the saved program in the list even when it is not running, so the
    // current choice is always visible.
    const Target t = engine_.target();
    if (!t.program.empty()) {
        bool found = false;
        for (const auto& p : programs_)
            found = found || sameExe(p.exe, t.program);
        if (!found) {
            ProgramInfo saved;
            saved.exe = t.program;
            saved.label = t.programLabel.empty() ? t.program : t.programLabel;
            programs_.insert(programs_.begin(), saved);
        }
    }

    SendMessageW(programList_, LB_RESETCONTENT, 0, 0);
    for (const auto& p : programs_) {
        std::wstring text = p.label;
        if (p.pid == 0)
            text += L", not running";
        else if (!p.hasAudio)
            text += L", has not played sound";
        SendMessageW(programList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(deviceList_, LB_RESETCONTENT, 0, 0);
    for (const auto& d : devices_)
        SendMessageW(deviceList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));

    selectCurrentTarget();

    if (announceCount) {
        say(std::to_wstring(programs_.size()) + L" programs and " + std::to_wstring(devices_.size())
            + L" output devices found.");
    }
}

void Gui::selectCurrentTarget()
{
    const Target t = engine_.target();
    SendMessageW(modeCombo_, CB_SETCURSEL, WPARAM(t.mode), 0);
    updateModeVisibility();

    int programIndex = programs_.empty() ? -1 : 0;
    for (size_t i = 0; i < programs_.size(); ++i) {
        if (!t.program.empty() && sameExe(programs_[i].exe, t.program))
            programIndex = int(i);
    }
    SendMessageW(programList_, LB_SETCURSEL, WPARAM(programIndex), 0);

    int deviceIndex = devices_.empty() ? -1 : 0;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].id == t.deviceId)
            deviceIndex = int(i);
    }
    SendMessageW(deviceList_, LB_SETCURSEL, WPARAM(deviceIndex), 0);
}

void Gui::useSelected()
{
    Target t = engine_.target();
    t.mode = selectedMode();
    if (t.mode == Mode::Capture) {
        const LRESULT i = SendMessageW(programList_, LB_GETCURSEL, 0, 0);
        if (i == LB_ERR || size_t(i) >= programs_.size()) {
            say(L"Select a program in the Program list first.");
            return;
        }
        t.program = programs_[size_t(i)].exe;
        t.programLabel = programs_[size_t(i)].label;
    } else {
        const LRESULT i = SendMessageW(deviceList_, LB_GETCURSEL, 0, 0);
        if (i == LB_ERR || size_t(i) >= devices_.size()) {
            say(L"Select a device in the Output device list first.");
            return;
        }
        t.deviceId = devices_[size_t(i)].id;
        t.deviceLabel = devices_[size_t(i)].name;
    }
    engine_.setTarget(t);
    seenTargetVersion_ = engine_.targetVersion();
    onTargetChanged_();
    if (!engine_.running())
        say(L"Saved. It will start when REAPER's audio is running.");
    // Otherwise the engine's status change ("Capturing ...") is announced.
}

void Gui::stopRouting()
{
    Target t = engine_.target();
    if (selectedMode() == Mode::Capture) {
        t.program.clear();
        t.programLabel.clear();
    } else {
        t.deviceId.clear();
        t.deviceLabel.clear();
    }
    t.mode = selectedMode();
    engine_.setTarget(t);
    seenTargetVersion_ = engine_.targetVersion();
    onTargetChanged_();
    say(L"Stopped.");
}

void Gui::announceLevel()
{
    float peak = 0.0f;
    for (float p : peakHistory_)
        peak = std::max(peak, p);
    peak = std::max(peak, engine_.takePeak());
    const wchar_t* what = selectedMode() == Mode::Capture ? L"Captured audio" : L"Sent audio";
    if (peak < 0.00001f) { // below -100 dB
        say(std::wstring(what) + L" is silent over the last 3 seconds.");
        return;
    }
    wchar_t text[128];
    swprintf(text, 128, L"%ls peak over the last 3 seconds: %.1f dB.", what, 20.0 * std::log10(peak));
    say(text);
}

void Gui::onTimer()
{
    peakHistory_[peakIndex_] = engine_.takePeak();
    peakIndex_ = (peakIndex_ + 1) % 6;

    if (engine_.targetVersion() != seenTargetVersion_) {
        // Changed from outside the window, e.g. a project was loaded.
        seenTargetVersion_ = engine_.targetVersion();
        refreshLists(false);
    }
    if (engine_.statusVersion() != seenStatusVersion_) {
        seenStatusVersion_ = engine_.statusVersion();
        say(engine_.status());
    }
}

void Gui::say(const std::wstring& text)
{
    SetWindowTextW(statusEdit_, text.c_str());
    announce(statusEdit_, text);
}

LRESULT CALLBACK Gui::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* gui = reinterpret_cast<Gui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!gui)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IdModeCombo:
            if (HIWORD(wp) == CBN_SELCHANGE)
                gui->updateModeVisibility();
            return 0;
        case IdProgramList:
        case IdDeviceList:
            if (HIWORD(wp) == LBN_DBLCLK)
                gui->useSelected();
            return 0;
        case IdShowAll:
            if (HIWORD(wp) == BN_CLICKED)
                gui->refreshLists(true);
            return 0;
        case IdUse:
            gui->useSelected();
            return 0;
        case IdStop:
            gui->stopRouting();
            return 0;
        case IdRefresh:
            gui->refreshLists(true);
            return 0;
        case IdLevel:
            gui->announceLevel();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == kTimerId)
            gui->onTimer();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        gui->hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Keyboard handling for every control, in case the host does not run its
// dialog manager over our window: Tab and Shift+Tab move between controls,
// Enter presses a button or uses the selected list item.
LRESULT CALLBACK Gui::controlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR refData)
{
    auto* gui = reinterpret_cast<Gui*>(refData);
    const int id = GetDlgCtrlID(hwnd);
    switch (msg) {
    case WM_GETDLGCODE:
        if (lp) {
            const MSG* m = reinterpret_cast<const MSG*>(lp);
            if (m->message == WM_KEYDOWN && m->wParam == VK_RETURN)
                return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTMESSAGE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_TAB) {
            if (HWND next = GetNextDlgTabItem(gui->hwnd_, hwnd, GetKeyState(VK_SHIFT) < 0))
                SetFocus(next);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (id == IdProgramList || id == IdDeviceList)
                gui->useSelected();
            else if (isButton(id))
                SendMessageW(hwnd, BM_CLICK, 0, 0);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\t' || wp == L'\r')
            return 0; // already handled on key down; avoid the error beep
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, controlProc, 1);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace aab
