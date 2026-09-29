#pragma once

#include <windows.h>

#include <string>

namespace aab {

// Asks the running screen reader (NVDA, JAWS, Narrator) to speak `text`,
// using a UI Automation notification raised from `hwnd`. Does nothing on
// systems without UI Automation notification support.
void announce(HWND hwnd, const std::wstring& text);

} // namespace aab
