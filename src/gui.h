#pragma once

// The plugin window, embedded by REAPER in the FX window. It only uses
// standard Windows controls (combo box, list boxes, buttons, a read-only
// edit), each preceded by a text label, so NVDA and JAWS can read them
// without any special scripts. Status changes are also spoken through UI
// Automation notifications.

#include "engine.h"
#include "win_audio.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace aab {

class Gui {
public:
    Gui(Engine& engine, std::function<void()> onTargetChanged);
    ~Gui();

    void setScale(double scale) { scale_ = scale; }
    void getSize(uint32_t* width, uint32_t* height) const;
    bool attach(HWND parent);
    void show(bool visible);

    static void unregisterWindowClass();

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK controlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR id, DWORD_PTR refData);

    void createControls();
    int px(int logical) const;
    Mode selectedMode() const;
    void updateModeVisibility();
    void refreshLists(bool announceCount);
    void selectCurrentTarget();
    void useSelected();
    void stopRouting();
    void announceLevel();
    void onTimer();
    void say(const std::wstring& text);

    Engine& engine_;
    std::function<void()> onTargetChanged_;
    double scale_ = 0.0; // 0 means use the system DPI
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;

    HWND modeCombo_ = nullptr;
    HWND programLabel_ = nullptr;
    HWND programList_ = nullptr;
    HWND deviceLabel_ = nullptr;
    HWND deviceList_ = nullptr;
    HWND showAllCheck_ = nullptr;
    HWND statusEdit_ = nullptr;

    std::vector<ProgramInfo> programs_;
    std::vector<DeviceInfo> devices_;
    uint32_t seenStatusVersion_ = 0;
    uint32_t seenTargetVersion_ = 0;
    float peakHistory_[6] = {}; // 6 x 500 ms = the last 3 seconds
    int peakIndex_ = 0;
};

} // namespace aab
