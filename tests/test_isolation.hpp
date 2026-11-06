#pragma once

#include <windows.h>
#include <initializer_list>
#include <iostream>
#include <string>

// Native GUI and clipboard checks run in a private window station (see
// tests/isolated_run.cpp), so a person's clipboard, screen and running Knock stay
// untouched. Outside one they skip, unless KNOCK_TEST_ALLOW_DESKTOP is set.
inline std::wstring object_name(HANDLE object) {
    wchar_t name[64]{};
    DWORD needed = 0;
    return GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &needed) ? name : L"";
}
inline bool in_private_window_station() {
    return object_name(GetProcessWindowStation()) != L"WinSta0";
}
inline bool on_hidden_desktop() {
    return object_name(GetThreadDesktop(GetCurrentThreadId())) != L"Default";
}

// allow_hidden_desktop: for a test that registers hotkeys, which a private station
// refuses, and that leaves the shared clipboard alone.
inline bool skip_outside_isolation(bool allow_hidden_desktop = false) {
    if (in_private_window_station() || (allow_hidden_desktop && on_hidden_desktop()) ||
        GetEnvironmentVariableW(L"KNOCK_TEST_ALLOW_DESKTOP", nullptr, 0)) return false;
    std::cout << "SKIP: run through isolated_run.exe, which keeps this test off the real desktop and clipboard.\n";
    return true;
}

// A native menu closes at once without the foreground, which a test copy off the
// real desktop never has. With KNOCK_TEST_MENU set, a copy using its own data
// folder holds the same item list open until told which item was chosen.
inline bool test_menu_open(HWND root) {
    return GetPropW(root, L"KnockTestMenu") != nullptr;
}
// Keys follow a native menu: Home, End, Up, Down over the rows, Return chooses.
inline void choose_test_menu(HWND root, std::initializer_list<UINT> keys) {
    const int rows = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(root, L"KnockTestMenu")));
    int row = 0;
    for (const UINT key : keys) {
        if (key == VK_HOME) row = 0;
        if (key == VK_END) row = rows - 1;
        if (key == VK_DOWN) row = (row + 1) % rows;
        if (key == VK_UP) row = (row + rows - 1) % rows;
        if (key == VK_RETURN) { PostMessageW(root, WM_APP + 31, static_cast<WPARAM>(row), 0); return; }
    }
    PostMessageW(root, WM_APP + 31, static_cast<WPARAM>(-1), 0);
}
