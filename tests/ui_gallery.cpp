// Drives a private copy of the real KnockTray through every screen and saves what
// it draws, with sample records. Run it on a hidden desktop, where windows can be
// captured; it asks the copy to leave the shared clipboard and real hotkeys alone:
//
//   isolated_run --desktop -- ui_gallery OUTPUT_FOLDER [DPI]
//
// DPI sets the scale the copy draws at (96, 120, 144); it is not the Windows display
// scale. Each image is the real window, captured with PrintWindow.
#include "knock/platform.hpp"
#include "test_isolation.hpp"

#include <objbase.h>
#include <gdiplus.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using knock::Json;
using namespace std::chrono_literals;

namespace {
std::filesystem::path output;
HWND root = nullptr;

void require(bool value, const std::string& text) { if (!value) throw std::runtime_error(text); }

template<class Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 4000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < until);
    return false;
}
Json send(const Json& request) {
    auto result = knock::send_command(request, false);
    require(result.value("ok", false), "IPC failed: " + result.dump());
    return result;
}
void settle() {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(root, WM_NULL, 0, 0, SMTO_BLOCK, 3000, &ignored);
    std::this_thread::sleep_for(150ms);
    SendMessageTimeoutW(root, WM_NULL, 0, 0, SMTO_BLOCK, 3000, &ignored);
}
struct Query { std::wstring caption; int id; std::vector<HWND> matches; };
BOOL CALLBACK collect(HWND window, LPARAM data) {
    auto& query = *reinterpret_cast<Query*>(data);
    if (query.id && GetDlgCtrlID(window) != query.id) return TRUE;
    if (!query.caption.empty()) {
        wchar_t text[512]{};
        GetWindowTextW(window, text, 512);
        if (query.caption != text) return TRUE;
    }
    if (knock::window_shown(window)) query.matches.push_back(window);
    return TRUE;
}
HWND find_caption(const std::wstring& caption, HWND parent = nullptr) {
    Query query{caption, 0, {}};
    EnumChildWindows(parent ? parent : root, collect, reinterpret_cast<LPARAM>(&query));
    return query.matches.empty() ? nullptr : query.matches.front();
}
HWND find_id(int id) {
    Query query{{}, id, {}};
    EnumChildWindows(root, collect, reinterpret_cast<LPARAM>(&query));
    return query.matches.empty() ? nullptr : query.matches.front();
}
void click(const std::wstring& caption) {
    HWND control = find_caption(caption);
    require(control != nullptr, "No button: " + knock::narrow(caption));
    SendMessageW(control, BM_CLICK, 0, 0);
    settle();
}
bool click_if(const std::wstring& caption) {
    HWND control = find_caption(caption);
    if (!control) return false;
    SendMessageW(control, BM_CLICK, 0, 0);
    settle();
    return true;
}
void set_text(int id, const std::wstring& value) {
    HWND control = find_id(id);
    require(control != nullptr, "No edit " + std::to_string(id));
    SendMessageW(control, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str()));
    if (GetWindowLongPtrW(control, GWL_STYLE) & ES_MULTILINE) {
        SendMessageW(control, EM_SETSEL, value.size(), static_cast<LPARAM>(value.size()));
        SendMessageW(control, WM_CHAR, L' ', 0);
        SendMessageW(control, WM_CHAR, VK_BACK, 0);
    }
    settle();
}
void press(int id, WPARAM key) {
    HWND control = find_id(id);
    require(control != nullptr, "No edit " + std::to_string(id));
    PostMessageW(control, WM_KEYDOWN, key, 0);
    settle();
}
void save_png(HWND window, const std::wstring& name) {
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RECT box{};
    GetWindowRect(window, &box);
    const int width = box.right - box.left, height = box.bottom - box.top;
    HDC dc = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    auto old = SelectObject(memory, bitmap);
    require(PrintWindow(window, memory, 0) != 0, "PrintWindow failed");
    SelectObject(memory, old);
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        constexpr CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        const auto path = (output / (name + L".png")).wstring();
        require(image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok, "Cannot save " + knock::narrow(name));
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, dc);
    std::cout << "saved " << knock::narrow(name) << " (" << width << "x" << height << ")\n" << std::flush;
}
void shot(const std::wstring& name) { settle(); save_png(root, name); }
void open_settings_from_menu() {
    PostMessageW(root, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(root), -1);
    require(eventually([] { return test_menu_open(root); }), "Menu did not open");
    choose_test_menu(root, {VK_HOME, VK_RETURN});
    eventually([] { return !test_menu_open(root); });
    settle();
}
void back_to_list() {
    for (int i = 0; i < 6; ++i) {
        if (!click_if(L"< BACK")) break;
    }
    settle();
}

void seed() {
    const auto now = knock::now_ms();
    (void)now;
    for (const auto* title : {"Ship the mixer tests", "Reply to the licence question", "Tidy the release notes"})
        send({{"op", "task.add"}, {"title", title}});
    auto tasks = send({{"op", "list"}, {"collection", "tasks"}}).at("items");
    send({{"op", "task.update"}, {"id", tasks[0].at("id")}, {"status", "in_progress"}, {"operator", "AUDIO-1"}, {"project", "CURRENT"}});
    send({{"op", "task.update"}, {"id", tasks[2].at("id")}, {"status", "done"}});

    const auto ideas = send({{"op", "note.add"}, {"title", "Project ideas"}, {"tags", {"Ideas", "Projects"}}}).at("note");
    send({{"op", "note.append"}, {"id", ideas.at("id")}, {"text", "A small synth that only plays what you hummed yesterday."}});
    send({{"op", "note.append"}, {"id", ideas.at("id")}, {"text", "Keep the tray icon. People like the tray icon.\nMaybe add a second one, just to see who notices."}});
    send({{"op", "note.add"}, {"title", "Websites"}, {"tags", {"Reference"}}});
    send({{"op", "note.add"}, {"title", "Shopping list"}});
    const auto old = send({{"op", "note.add"}, {"title", "Old draft"}}).at("note");
    send({{"op", "note.delete"}, {"id", old.at("id")}});

    // Finished jobs, oldest first, so the newest is at the top of Done.
    send({{"op", "done"}, {"chat", "SITE-4"}, {"summary", "Landing page copy updated. Nothing deployed."}});
    send({{"op", "done"}, {"chat", "DOCS-2"}, {"summary", "Guide rewritten.\nTwo sections still need screenshots."}, {"report", "C:\\Projects\\Docs\\report.md"}});
    send({{"op", "done"}, {"chat", "BUILD-7"}, {"summary", "Release build finished and all 40 tests pass.\nThe installer is in out\\Knock. Nothing was committed."},
          {"report", "C:\\Projects\\Current\\reports\\build-7.md"}});

    // A finished command with a log, a running agent and a status line.
    send({{"op", "notify"}, {"chat", "RENDER-3"}, {"message", "Nightly render finished; 12 frames differ from the reference."}});
    knock::send_command({{"op", "agent.start"}, {"chat", "KNOCK-2"}, {"title", "Redesign the Knock window"}, {"report", "C:\\qwoi\\Knock-work\\report.md"}}, false);
    knock::send_command({{"op", "agent.update"}, {"chat", "KNOCK-2"}, {"status", "Checking every screen at 125 percent"}}, false);
    knock::send_command({{"op", "agent.start"}, {"chat", "CURRENT-11"}, {"title", "Measure Current startup on the slow disk"}}, false);

    // Two requests waiting for an answer.
    send({{"op", "request.create"}, {"chat", "AUDIO-1"}, {"kind", "question"}, {"timeout_seconds", 3000},
          {"message", "The mixer has two valid meanings for 'solo'. Should a soloed bus also mute the send returns?"}});
    send({{"op", "request.create"}, {"chat", "EDITOR-9"}, {"timeout_seconds", 1500}, {"lease_seconds", 300},
          {"message", "May I use the mouse and keyboard for the drag and drop tests? About 3 minutes."}});
}

void gallery() {
    shot(L"01-agents");
    if (HWND done = find_caption(L"DONE (3)")) {
        SendMessageW(done, BM_CLICK, 0, 0);
        shot(L"02-agents-done-open");
    }
    if (click_if(L"COPY REPORT")) { save_png(root, L"03-copy-report-confirmation"); settle(); }
    if (HWND done = find_caption(L"DONE (3)")) { SendMessageW(done, BM_CLICK, 0, 0); settle(); }

    click(L"CUSTOM");
    set_text(35, L"Not while I'm on a call. Keep going with the headless tests and ask again at four.");
    shot(L"04-custom-reply");
    click(L"CANCEL");

    // Tasks: click the first row of the drawn list.
    PostMessageW(GetDlgItem(root, 61), WM_LBUTTONUP, 0, MAKELPARAM(12, 12));
    if (eventually([] { return find_id(33) != nullptr; })) {
        shot(L"05-task");
        click(L"< BACK");
    }

    // Notes: open a conversation by command, then its sub-pages.
    set_text(30, L"/find-note Project ideas");
    press(30, VK_RETURN);
    shot(L"06-find-note");
    press(51, VK_RETURN);
    if (eventually([] { return find_id(37) != nullptr; })) {
        shot(L"07-note");
        click(L"TAGS");
        shot(L"08-note-tags");
        click(L"DONE");
        click(L"EDIT");
        shot(L"09-note-details");
        click(L"EDIT");
        click(L"< BACK");
    }
    set_text(30, L"/search-notes tray");
    press(30, VK_RETURN);
    std::this_thread::sleep_for(300ms);
    shot(L"10-search-notes");
    click(L"< BACK");
    set_text(30, L"/delete-note");
    press(30, VK_RETURN);
    shot(L"11-delete-note");
    click(L"< BACK");
    set_text(30, L"/trash");
    press(30, VK_RETURN);
    shot(L"12-trash");
    click(L"< BACK");
    set_text(30, L"/");
    shot(L"13-note-commands");
    set_text(30, L"");
    click(L"#");
    shot(L"14-tag-filter");
    click(L"DONE");

    // Settings and its pages.
    if (!click_if(L"SETTINGS")) open_settings_from_menu();
    shot(L"15-settings");
    PostMessageW(find_caption(L"CHANGE"), BM_CLICK, 0, 0);
    HWND shortcut = nullptr;
    if (eventually([&] { shortcut = FindWindowW(L"Knock.Shortcut.v1", nullptr); return shortcut != nullptr; })) {
        settle();
        save_png(shortcut, L"16-shortcut-dialog");
        SendMessageW(find_caption(L"CANCEL", shortcut), BM_CLICK, 0, 0);
        settle();
    }
    click(L"MORE >");
    shot(L"17-preferences");
    click(L"MANAGE");
    shot(L"18-saved-replies");
    click(L"ADD");
    set_text(44, L"Finish the headless tests first, then ask again.");
    shot(L"19-saved-reply-edit");
    click(L"CANCEL");
    back_to_list();

    // Status, a lease and the quiet timer at once.
    set_text(45, L"Out for coffee. Headless work is fine.");
    click(L"AWAY");
    click(L"YES");
    std::this_thread::sleep_for(400ms);
    shot(L"20-away-with-lease");
    click_if(L"RELEASE");
    settle();

    // The awkward cases: a long name, a long message, a non-Latin name, and a row read in full.
    send({{"op", "request.create"}, {"chat", "A-VERY-LONG-JOB-NAME-THAT-KEEPS-GOING-AND-GOING-AND-GOING-123456"}, {"timeout_seconds", 600},
          {"message", std::string("May I use the mouse and keyboard? ") + std::string(700, 'x') + " That is the end of the message."}});
    send({{"op", "agent.start"}, {"chat", "\xE4\xBD\x9C\xE6\xA5\xAD-1"}, {"title", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x82\xB8\xE3\x83\xA7\xE3\x83\x96"}});
    shot(L"24-long-text");
    {
        RECT box{};
        HWND list = FindWindowExW(root, nullptr, L"Knock.Content.v1", nullptr);
        GetClientRect(list, &box);
        SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(box.right / 3, 30));
        shot(L"25-read-all");
        SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(box.right / 3, 30));
        settle();
    }
    click(L"HIDE NOTES");
    shot(L"21-notes-collapsed");
    click(L"SHOW NOTES");

    if (!click_if(L"SETTINGS")) open_settings_from_menu();
    set_text(41, L"900");
    set_text(42, L"300");
    click(L"APPLY");
    shot(L"22-settings-minimum");
    click(L"< BACK");
    shot(L"23-agents-minimum");
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) { std::cerr << "usage: ui_gallery OUTPUT_FOLDER [DPI]\n"; return 2; }
    if (skip_outside_isolation(true)) return 77;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    HANDLE process = nullptr;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        output = argv[1];
        std::filesystem::create_directories(output);
        const auto data = output / L"data";
        std::error_code ignored;
        std::filesystem::remove_all(data, ignored);
        SetEnvironmentVariableW(L"KNOCK_DATA_DIR", data.c_str());
        SetEnvironmentVariableW(L"KNOCK_TEST_MENU", L"1");
        SetEnvironmentVariableW(L"KNOCK_TEST_NO_CLIPBOARD", L"1");
        SetEnvironmentVariableW(L"KNOCK_TEST_NO_HOTKEY", L"1");
        if (argc > 2) SetEnvironmentVariableW(L"KNOCK_TEST_DPI", argv[2]);
        const auto exe = knock::executable_path().parent_path() / L"KnockTray.exe";
        auto line = knock::command_line({exe.wstring(), L"--show"});
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               exe.parent_path().c_str(), &startup, &child) != 0, "Cannot start KnockTray");
        CloseHandle(child.hThread);
        process = child.hProcess;
        require(eventually([&] {
            root = FindWindowW(knock::tray_window_class().c_str(), nullptr);
            return root && find_id(45) != nullptr;
        }), "KnockTray did not open");
        require(eventually([] { return knock::send_command({{"op", "status"}}, false).value("ok", false); }), "No IPC");
        // Keep it open: the shortcut message pins the window.
        SendMessageW(root, WM_CLOSE, 0, 0);
        SendMessageW(root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        require(eventually([] { return knock::window_shown(root); }), "Window did not stay open");
        shot(L"00-empty");
        seed();
        gallery();
        SendMessageW(root, WM_QUERYENDSESSION, 0, 0);
        PostMessageW(root, WM_ENDSESSION, TRUE, 0);
        WaitForSingleObject(process, 5000);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (root && IsWindow(root)) {
            try { save_png(root, L"failure"); } catch (...) {}
            PostMessageW(root, WM_ENDSESSION, TRUE, 0);
        }
        if (process) WaitForSingleObject(process, 5000);
        return 1;
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
