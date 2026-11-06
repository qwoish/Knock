// Runs a command away from the person using this PC.
//
//   isolated_run [--desktop] [--name NAME] -- PROGRAM ARGS...
//
// By default the command gets a private window station: its own clipboard, atom
// table and desktop, no shell and no tray area, so nothing it does is visible or
// reaches the real clipboard. Screen metrics there are a small fixed size.
//
// --desktop instead uses a hidden desktop on the real window station. It sees the
// real monitors, so a window is laid out as it would be on screen, but the
// clipboard is shared: use it only for commands that leave the clipboard alone.
//
// Child processes inherit the same desktop. KNOCK_DATA_DIR is what keeps a test
// copy away from a running Knock; this only keeps it off the screen.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

std::wstring quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\r\n\v\f\"") == std::wstring::npos) return argument;
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

int fail(const char* what) {
    std::cerr << "isolated_run: " << what << " failed (Windows error " << GetLastError() << ")\n";
    return 125;
}

}

int wmain(int argc, wchar_t* argv[]) {
    bool hidden_desktop = false;
    std::wstring name = L"Knock" + std::to_wstring(GetCurrentProcessId());
    int first = 1;
    for (; first < argc; ++first) {
        const std::wstring argument = argv[first];
        if (argument == L"--") { ++first; break; }
        if (argument == L"--desktop") hidden_desktop = true;
        else if (argument == L"--name" && first + 1 < argc) name = argv[++first];
        else { std::cerr << "usage: isolated_run [--desktop] [--name NAME] -- PROGRAM ARGS...\n"; return 125; }
    }
    if (first >= argc) { std::cerr << "usage: isolated_run [--desktop] [--name NAME] -- PROGRAM ARGS...\n"; return 125; }

    std::wstring command_line;
    for (int index = first; index < argc; ++index) {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += quote(argv[index]);
    }

    constexpr ACCESS_MASK all = GENERIC_ALL;
    HWINSTA station = nullptr;
    HDESK desktop = nullptr;
    std::wstring target;
    if (hidden_desktop) {
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, all, nullptr);
        if (!desktop) return fail("CreateDesktop");
        target = L"WinSta0\\" + name;
    } else {
        HWINSTA original = GetProcessWindowStation();
        station = CreateWindowStationW(name.c_str(), 0, all, nullptr);
        if (!station) return fail("CreateWindowStation");
        // Without this flag windows in the station never become visible.
        USEROBJECTFLAGS flags{};
        flags.dwFlags = WSF_VISIBLE;
        if (!SetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags))) return fail("SetUserObjectInformation");
        if (!SetProcessWindowStation(station)) return fail("SetProcessWindowStation");
        desktop = CreateDesktopW(L"Default", nullptr, nullptr, 0, all, nullptr);
        const DWORD error = GetLastError();
        SetProcessWindowStation(original);
        if (!desktop) { SetLastError(error); return fail("CreateDesktop"); }
        target = name + L"\\Default";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = target.data();
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        return fail("CreateProcess");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 125;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseDesktop(desktop);
    if (station) CloseWindowStation(station);
    return static_cast<int>(code);
}
