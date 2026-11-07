// The agents list end to end: the real knock.exe talking to a private KnockTray.
// Run through isolated_run.exe, like the other GUI tests.
#include "knock/platform.hpp"
#include "test_isolation.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using knock::Json;
using namespace std::chrono_literals;

namespace {
void require(bool value, const std::string& text) { if (!value) throw std::runtime_error(text); }

template<class Predicate>
void eventually(Predicate predicate, const std::string& text, std::chrono::milliseconds timeout = 5000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error("Timed out: " + text);
}
std::wstring text(HWND window) {
    std::wstring value(4096, L'\0');
    DWORD_PTR written = 0;
    require(SendMessageTimeoutW(window, WM_GETTEXT, value.size(), reinterpret_cast<LPARAM>(value.data()),
                                SMTO_ABORTIFHUNG, 3000, &written) != 0, "Window text timed out");
    value.resize(written);
    return value;
}
struct Query { std::wstring caption; std::vector<HWND> matches; };
BOOL CALLBACK collect(HWND window, LPARAM data) {
    auto& query = *reinterpret_cast<Query*>(data);
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (_wcsicmp(type, L"BUTTON") == 0 && knock::window_shown(window) &&
        (query.caption.empty() || text(window) == query.caption)) query.matches.push_back(window);
    return TRUE;
}
std::vector<HWND> buttons(HWND root, const std::wstring& caption = {}) {
    Query query{caption, {}};
    EnumChildWindows(root, collect, reinterpret_cast<LPARAM>(&query));
    return query.matches;
}
void click(HWND root, const std::wstring& caption) {
    const auto found = buttons(root, caption);
    require(found.size() == 1, "Expected one button " + knock::narrow(caption) + ", got " + std::to_string(found.size()));
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(found.front(), BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 3000, &ignored);
}
struct Output { int code; Json json; };

struct App {
    std::filesystem::path directory;
    std::filesystem::path folder = knock::executable_path().parent_path();
    HANDLE process = nullptr;
    HWND root = nullptr;
    HWND list = nullptr;
    // dpi draws at that scale; small opens at the smallest window size.
    explicit App(const wchar_t* dpi = nullptr, bool small = false) {
        wchar_t temp[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temp) != 0, "No temp folder");
        directory = std::filesystem::path(temp) / (L"knock-agents-ui-" + knock::widen(knock::make_id()));
        require(SetEnvironmentVariableW(L"KNOCK_DATA_DIR", directory.c_str()) != 0, "Cannot isolate data");
        SetEnvironmentVariableW(L"KNOCK_TEST_MENU", L"1");
        SetEnvironmentVariableW(L"KNOCK_TEST_DPI", dpi);
        if (small) {
            std::filesystem::create_directories(directory);
            std::ofstream(directory / L"settings.json") << R"({"window":{"bar_width":900,"bar_height":300}})";
        }
        const auto exe = folder / L"KnockTray.exe";
        auto line = knock::command_line({exe.wstring(), L"--show"});
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                               folder.c_str(), &startup, &child) != 0, "Cannot start KnockTray");
        CloseHandle(child.hThread);
        process = child.hProcess;
        eventually([&] {
            root = FindWindowW(knock::tray_window_class().c_str(), nullptr);
            return root && !buttons(root, L"AT PC").empty();
        }, "the tray opens");
        list = FindWindowExW(root, nullptr, L"Knock.Content.v1", nullptr);
        require(list != nullptr, "No agents list");
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(root, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 3000, &ignored);
        SendMessageTimeoutW(root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'), SMTO_ABORTIFHUNG, 3000, &ignored);
        eventually([&] { return knock::window_shown(root); }, "the window stays open");
    }
    ~App() {
        if (!process) return;
        DWORD_PTR ignored = 0;
        if (root && IsWindow(root)) {
            SendMessageTimeoutW(root, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 3000, &ignored);
            PostMessageW(root, WM_ENDSESSION, TRUE, 0);
        }
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    // Runs the real CLI, as an agent does.
    Output cli(const std::vector<std::wstring>& arguments) const {
        auto [code, bytes] = raw_cli(arguments);
        return {code, Json::parse(bytes)};
    }
    std::pair<int, std::string> raw_cli(const std::vector<std::wstring>& arguments) const {
        std::vector<std::wstring> full{(folder / L"knock.exe").wstring()};
        full.insert(full.end(), arguments.begin(), arguments.end());
        auto line = knock::command_line(full);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        require(CreatePipe(&read, &write, &inherit, 0) != 0, "No pipe");
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write;
        startup.hStdError = write;
        PROCESS_INFORMATION child{};
        require(CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != 0, "No CLI");
        CloseHandle(child.hThread);
        CloseHandle(write);
        std::string bytes;
        char buffer[4096];
        DWORD count = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count) bytes.append(buffer, count);
        CloseHandle(read);
        require(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, "CLI timed out");
        DWORD code = 0;
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hProcess);
        return {static_cast<int>(code), bytes};
    }
    Json good(const std::vector<std::wstring>& arguments) const {
        auto output = cli(arguments);
        require(output.code == 0 && output.json.value("ok", false), "CLI failed: " + output.json.dump());
        return output.json;
    }
    void shows(const std::wstring& expected) const {
        try {
            eventually([&] { return text(list) == expected; }, "list order");
        } catch (const std::exception&) {
            throw std::runtime_error("The list shows \"" + knock::narrow(text(list)) + "\", expected \"" + knock::narrow(expected) + "\"");
        }
    }
};
void passed(const char* label) { std::cout << "PASS " << label << '\n' << std::flush; }
// Rows can scroll vertically; nothing may ever stick out sideways, overlap its
// neighbour in a row, or (for controls outside the list) leave its parent.
void everything_fits(HWND root, HWND list, const std::string& where) {
    std::vector<HWND> controls = buttons(root);
    RECT list_box{};
    GetWindowRect(list, &list_box);
    std::vector<RECT> boxes;
    for (HWND control : controls) {
        RECT box{}, parent{};
        GetWindowRect(control, &box);
        GetWindowRect(GetParent(control), &parent);
        const bool scrolls = GetParent(control) == list && knock::window_shown(list);
        require(box.left >= parent.left && box.right <= parent.right && box.right > box.left && box.bottom > box.top &&
                (scrolls || (box.top >= parent.top && box.bottom <= parent.bottom)),
                where + ": a control leaves its parent: " + knock::narrow(text(control)));
        // A row scrolled past the edge of the list is clipped there, not overlapping.
        RECT seen = box;
        if (scrolls && !IntersectRect(&seen, &box, &list_box)) seen = RECT{};
        boxes.push_back(seen);
    }
    for (std::size_t i = 0; i < boxes.size(); ++i)
        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
            RECT overlap{};
            require(!IntersectRect(&overlap, &boxes[i], &boxes[j]),
                    where + ": controls overlap: " + knock::narrow(text(controls[i])) + " and " + knock::narrow(text(controls[j])));
        }
}
void settle(HWND root) {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(root, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, &ignored);
    std::this_thread::sleep_for(100ms);
    SendMessageTimeoutW(root, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, &ignored);
}
void fits_at(const wchar_t* dpi, bool small) {
    App app(dpi, small);
    const auto label = knock::narrow(dpi) + (small ? " smallest" : " default");
    const std::string long_title(150, 'x');
    app.good({L"start", L"--chat", L"A-VERY-LONG-JOB-NAME-THAT-KEEPS-GOING-AND-GOING-AND-GOING-123456", L"--title", knock::widen(long_title)});
    app.good({L"update", L"--chat", L"WORK-1", L"--status", L"Measuring"});
    app.good({L"request", L"--chat", L"ASK-1", L"--message", knock::widen(std::string(900, 'y')), L"--detach"});
    app.good({L"request", L"--chat", L"ASK-2", L"--kind", L"question", L"--message", L"Short question?", L"--detach"});
    app.good({L"notify", L"--chat", L"NOTICE-1", L"--message", L"Build finished."});
    app.good({L"done", L"--chat", L"OLD-1", L"--summary", L"Finished.", L"--report", L"C:\\a\\very\\long\\path\\to\\a\\report\\that\\keeps\\going\\report.md"});
    app.good({L"quiet", L"--chat", L"A-VERY-LONG-JOB-NAME-THAT-KEEPS-GOING-AND-GOING-AND-GOING-123456", L"--minutes", L"30", L"--reason", knock::widen(long_title)});
    settle(app.root);
    everything_fits(app.root, app.list, label + " agents");
    click(app.root, L"DONE (1)");
    settle(app.root);
    everything_fits(app.root, app.list, label + " agents with Done open");
    click(app.root, L"DONE (1)");
    click(app.root, L"SETTINGS");
    settle(app.root);
    everything_fits(app.root, app.list, label + " settings");
    click(app.root, L"MORE >");
    settle(app.root);
    everything_fits(app.root, app.list, label + " preferences");
    click(app.root, L"MANAGE");
    settle(app.root);
    everything_fits(app.root, app.list, label + " saved replies");
    click(app.root, L"< BACK"); click(app.root, L"< BACK"); click(app.root, L"< BACK");
    settle(app.root);
    std::cout << "PASS everything fits: " << label << '\n' << std::flush;
}
// Timestamps are in milliseconds; keep start order unambiguous.
void tick() { std::this_thread::sleep_for(5ms); }

std::string lease_holder() {
    const auto lease = knock::send_command({{"op", "status"}}, false).at("lease");
    return lease.is_null() ? std::string() : lease.at("chat").get<std::string>();
}

// The real CLI against the real window: the line for the mouse and keyboard.
void input_queue_on_the_panel() {
    App app;
    const auto first = app.good({L"request", L"--chat", L"LEASE-1", L"--message", L"May I?", L"--lease", L"120", L"--detach"});
    app.shows(L"Agents: WAITING LEASE-1");
    require(!first.contains("queue"), "Nothing is ahead of the first request");
    click(app.root, L"YES");
    eventually([&] { return lease_holder() == "LEASE-1"; }, "the first lease");

    const auto second = app.good({L"request", L"--chat", L"QUEUE-2", L"--message", L"May I, next?", L"--lease", L"60", L"--detach"});
    tick();
    const auto third = app.good({L"request", L"--chat", L"QUEUE-3", L"--message", L"And me?", L"--lease", L"60", L"--detach"});
    require(second.at("queue").at("position") == 1 && second.at("queue").at("holder") == "LEASE-1" &&
            second.at("queue").at("expected_wait_seconds").get<int>() > 100, "Wrong place for the second: " + second.dump());
    require(third.at("queue").at("position") == 2 && third.at("queue").at("expected_wait_seconds").get<int>() > 160,
            "Wrong place for the third: " + third.dump());
    app.shows(L"Agents: QUEUED QUEUE-2 | QUEUED QUEUE-3 | WORKING LEASE-1");
    for (HWND yes : buttons(app.root, L"YES")) require(!IsWindowEnabled(yes), "A queued row can be approved");
    require(buttons(app.root, L"NO").size() == 2 && buttons(app.root, L"RELEASE").size() == 1, "The queued rows lost their other answers");
    passed("a second input request waits in a queue on the panel, and its JSON says its place and wait");

    tick();
    app.good({L"request", L"--chat", L"ASK-4", L"--kind", L"question", L"--message", L"A question?", L"--detach"});
    app.shows(L"Agents: WAITING ASK-4 | QUEUED QUEUE-2 | QUEUED QUEUE-3 | WORKING LEASE-1");
    passed("a question is not queued and goes above the line");

    click(app.root, L"RELEASE");
    app.shows(L"Agents: WAITING ASK-4 | WAITING QUEUE-2 | QUEUED QUEUE-3 | WORKING LEASE-1");
    require(lease_holder().empty(), "Release granted the next by itself");
    passed("release sends only the next in line to the person");

    // Away: the line is granted in turn, and a waiter holds its place through Away.
    click(app.root, L"AWAY");
    const auto cancel_next = app.cli({L"cancel", knock::widen(second.at("request").at("id").get<std::string>())});
    require(cancel_next.code == 0, "Cancel failed");
    eventually([&] { return lease_holder() == "QUEUE-3"; }, "the away grant, in a lease held by QUEUE-3");
    const auto granted = app.cli({L"result", knock::widen(third.at("request").at("id").get<std::string>())});
    require(granted.code == 0 && granted.json.at("request").at("granted_by") == "away", "The away grant was not usable: " + granted.json.dump());
    auto waiter = std::async(std::launch::async, [&] {
        return app.cli({L"request", L"--chat", L"LATE-5", L"--message", L"Me too?", L"--lease", L"30", L"--timeout", L"8"});
    });
    eventually([&] {
        return knock::send_command({{"op", "status"}}, false).at("input_queue").size() == 1;
    }, "the waiter joins the line");
    require(waiter.wait_for(100ms) == std::future_status::timeout, "A queued request returned because of Away");
    const auto released = app.cli({L"release", knock::widen(third.at("request").at("id").get<std::string>())});
    require(released.code == 0, "Release failed");
    const auto late = waiter.get();
    require(late.code == 0 && late.json.at("request").at("status") == "approved" && late.json.at("request").at("granted_by") == "away",
            "The blocked request was not granted when its turn came: " + late.json.dump());
    app.good({L"release", knock::widen(late.json.at("request").at("id").get<std::string>())});
    passed("away grants the line in turn; a blocked agent is woken without polling");
    click(app.root, L"AT PC");
    const auto keep_lease = app.good({L"request", L"--chat", L"KEEP-6", L"--message", L"At PC again", L"--detach"});
    require(keep_lease.at("request").at("status") == "pending", "At PC granted a request");
    app.good({L"cancel", knock::widen(keep_lease.at("request").at("id").get<std::string>())});
}

void quiet_windows_on_the_panel() {
    App app;
    const auto closed = app.raw_cli({L"info"});
    require(closed.first == 0 && closed.second.find("Agents checked in: none.") != std::string::npos &&
            closed.second.find("Quiet window: none open.") != std::string::npos, "info did not report an empty PC");
    app.good({L"start", L"--chat", L"OTHER-1", L"--title", L"Another job on this PC"});
    const auto first = app.good({L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5", L"--reason", L"Timing the startup"});
    require(first.at("quiet").at("status") == "open" && first.at("quiet_window").at("chat") == "RUN-1", "Wrong window: " + first.dump());
    app.shows(L"Agents: WORKING OTHER-1 | QUIET WINDOW RUN-1");
    require(buttons(app.root, L"END QUIET").size() == 1, "The panel cannot end the window");
    everything_fits(app.root, app.list, "with a quiet window");

    const auto listed = app.raw_cli({L"info"});
    require(listed.first == 0 && listed.second.find("- OTHER-1: Another job on this PC (started ") != std::string::npos &&
            listed.second.find("Quiet window: RUN-1 until ") != std::string::npos &&
            listed.second.find("Timing the startup") != std::string::npos, "info does not name the window and the agents: " + listed.second);
    const auto asked = app.good({L"request", L"--chat", L"ASK-1", L"--kind", L"question", L"--message", L"Is it open?", L"--detach"});
    require(asked.at("quiet_window").at("open") == true && asked.at("quiet_window").at("chat") == "RUN-1", "A request's JSON does not say a window is open");
    app.good({L"cancel", knock::widen(asked.at("request").at("id").get<std::string>())});
    passed("knock quiet opens a window; info, the panel and every request's JSON name it");

    const auto second = app.good({L"quiet", L"--chat", L"RUN-2", L"--minutes", L"1", L"--reason", L"The second run", L"--detach"});
    require(second.at("quiet").at("status") == "waiting" && second.at("queue").at("position") == 1 && second.at("queue").at("holder") == "RUN-1",
            "The second did not wait: " + second.dump());
    const auto status = app.good({L"quiet", L"--status"});
    require(status.at("waiting").size() == 1 && status.at("waiting")[0].at("chat") == "RUN-2", "--status does not list the line");
    // The same command, blocking, picks up its place; a release lets it through.
    auto waiter = std::async(std::launch::async, [&] {
        return app.cli({L"quiet", L"--chat", L"RUN-2", L"--minutes", L"1", L"--reason", L"The second run", L"--timeout", L"8"});
    });
    require(waiter.wait_for(200ms) == std::future_status::timeout, "The second window did not block");
    const auto released = app.good({L"quiet", L"--release", L"--chat", L"RUN-1"});
    require(released.at("quiet").at("end_reason") == "released", "Wrong release");
    const auto opened = waiter.get();
    require(opened.code == 0 && opened.json.at("quiet").at("status") == "open" && opened.json.at("quiet").at("chat") == "RUN-2",
            "The waiter did not get its window: " + opened.json.dump());
    app.shows(L"Agents: WORKING ASK-1 | WORKING OTHER-1 | QUIET WINDOW RUN-2");
    passed("a second window waits like a lease and opens when the first is released");

    // The third times out in its place; the person can end what is open.
    const auto third = app.cli({L"quiet", L"--chat", L"RUN-3", L"--minutes", L"1", L"--reason", L"Too late", L"--timeout", L"1"});
    require(third.code == 4 && third.json.at("quiet").at("end_reason") == "timed_out", "Wrong exit for a window that gave up: " + third.json.dump());
    click(app.root, L"END QUIET");
    eventually([&] { return buttons(app.root, L"END QUIET").empty(); }, "END QUIET ends the window");
    app.shows(L"Agents: WORKING ASK-1 | WORKING OTHER-1");
    require(app.good({L"quiet", L"--status"}).at("quiet_window").at("open") == false, "The window stayed open");
    passed("a waiter that gives up exits 4; the panel can end a window");
}
}

int main() {
    if (skip_outside_isolation()) return 77;
    try {
        {
        App app;
        const auto first = app.good({L"start", L"--chat", L"FIRST-1", L"--title", L"The first job", L"--report", L"report.md"});
        require(first.at("agent").at("status") == "working" && first.contains("presence") &&
                std::filesystem::path(first.at("agent").at("report").get<std::string>()).is_absolute(),
                "knock start printed the wrong record: " + first.dump());
        app.shows(L"Agents: WORKING FIRST-1");
        passed("knock start shows a job as working from that moment");

        tick();
        app.good({L"start", L"--chat", L"SECOND-2", L"--title", L"The second job"});
        app.shows(L"Agents: WORKING SECOND-2 | WORKING FIRST-1");
        passed("the newest working job is at the top");

        const auto status = app.good({L"update", L"--chat", L"FIRST-1", L"--status", L"Running the tests"});
        require(status.at("agent").at("status_text") == "Running the tests" && status.at("agent").at("id") == first.at("agent").at("id"),
                "knock update printed the wrong record");
        std::this_thread::sleep_for(100ms);
        app.shows(L"Agents: WORKING SECOND-2 | WORKING FIRST-1");
        passed("a status update changes one line and never reorders the list");

        const auto waiting = app.good({L"request", L"--chat", L"THIRD-3", L"--kind", L"question", L"--message", L"Keep the old mixer?", L"--detach"});
        app.shows(L"Agents: WAITING THIRD-3 | WORKING SECOND-2 | WORKING FIRST-1");
        require(buttons(app.root, L"YES").size() == 1, "The waiting row cannot be answered");
        passed("an agent waiting for an answer goes above working agents");

        // Finished jobs collapse into Done, newest first; a job that never checked in appears too.
        app.good({L"done", L"--chat", L"SECOND-2", L"--summary", L"Second finished."});
        app.shows(L"Agents: WAITING THIRD-3 | WORKING FIRST-1 (1 done, closed)");
        tick();
        const auto finished = app.good({L"done", L"--chat", L"FIRST-1", L"--summary", L"First finished."});
        require(finished.at("done").at("report") == first.at("agent").at("report"), "done lost the report named at check-in");
        app.shows(L"Agents: WAITING THIRD-3 (2 done, closed)");
        tick();
        app.good({L"done", L"--chat", L"NEVER-4", L"--summary", L"Never checked in."});
        app.shows(L"Agents: WAITING THIRD-3 (3 done, closed)");
        require(buttons(app.root, L"COPY REPORT").empty(), "A closed Done section shows finished jobs");
        click(app.root, L"DONE (3)");
        app.shows(L"Agents: WAITING THIRD-3 | DONE NEVER-4 | DONE FIRST-1 | DONE SECOND-2");
        require(buttons(app.root, L"COPY REPORT").size() == 3, "Done rows have no COPY REPORT");
        passed("finished jobs collapse into a Done section that opens newest first");

        // Archived rows stay out of sight, and a job that starts again comes back to work.
        const auto rows = app.good({L"inbox"}).at("inbox");
        for (const auto& record : rows)
            if (record.at("chat") == "NEVER-4")
                require(knock::send_command({{"op", "item.dismiss"}, {"kind", "inbox"}, {"id", record.at("id")}}, false).value("ok", false), "Dismiss failed");
        app.shows(L"Agents: WAITING THIRD-3 | DONE FIRST-1 | DONE SECOND-2");
        tick();
        app.good({L"start", L"--chat", L"SECOND-2", L"--title", L"The second job, again"});
        app.shows(L"Agents: WAITING THIRD-3 | WORKING SECOND-2 | DONE FIRST-1");
        click(app.root, L"DONE (1)");
        app.shows(L"Agents: WAITING THIRD-3 | WORKING SECOND-2 (1 done, closed)");
        passed("archived rows stay hidden; starting again returns a job to work");

        // Bad use reports through JSON and exit 5, and never touches the list.
        for (const auto& arguments : std::vector<std::vector<std::wstring>>{
                {L"start", L"--chat", L"BAD"}, {L"update", L"--chat", L"BAD"}, {L"start", L"--title", L"x"}}) {
            const auto output = app.cli(arguments);
            require(output.code == 5 && !output.json.value("ok", true), "Bad arguments did not fail cleanly");
        }
        std::this_thread::sleep_for(100ms);
        app.shows(L"Agents: WAITING THIRD-3 | WORKING SECOND-2 (1 done, closed)");

        // Answering still takes the GUI, and a check-in grants nothing.
        const auto id = waiting.at("request").at("id");
        require(!knock::send_command({{"op", "request.resolve"}, {"id", id}, {"decision", "approved"}}, false).value("ok", true),
                "A CLI answered a request");
        const auto pending = app.cli({L"result", knock::widen(id.get<std::string>())});
        require(pending.code == 7 && pending.json.at("request").at("status") == "pending" &&
                knock::send_command({{"op", "status"}}, false).at("lease").is_null(), "A check-in changed a request or granted input");
        passed("bad arguments fail cleanly; only the GUI answers requests, and a check-in grants nothing");
        }
        input_queue_on_the_panel();
        quiet_windows_on_the_panel();
        // The window at each scale it can draw at without changing the display.
        for (const auto* dpi : {L"96", L"120", L"144", L"192"})
            for (const bool small : {false, true}) fits_at(dpi, small);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
