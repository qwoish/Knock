#include "knock/runner.hpp"
#include "knock/agent_info.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
unsigned int platform_calls = 0;
bool fake_ipc_enabled = false;
std::vector<knock::Json> ipc_commands;
std::vector<knock::Json> ipc_results;
std::size_t ipc_result_index = 0;
bool last_start_app = true;
std::optional<knock::RunOptions> started_job;

[[noreturn]] void unexpected_platform_call() {
    ++platform_calls;
    throw std::runtime_error("Offline help attempted to use the platform");
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Capture {
public:
    Capture() : previous_(std::cout.rdbuf(stream_.rdbuf())) {}
    ~Capture() { std::cout.rdbuf(previous_); }
    std::string text() const { return stream_.str(); }
private:
    std::ostringstream stream_;
    std::streambuf* previous_;
};

class FakeIpc {
public:
    explicit FakeIpc(std::vector<knock::Json> responses) {
        ipc_commands.clear();
        ipc_results = std::move(responses);
        ipc_result_index = 0;
        fake_ipc_enabled = true;
    }
    ~FakeIpc() { fake_ipc_enabled = false; }
};

std::string invoke(const std::vector<std::wstring>& arguments) {
    Capture capture;
    require(knock::run_cli(arguments) == 0, "Offline guide did not succeed");
    return capture.text();
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void rejects(const std::vector<std::wstring>& arguments) {
    Capture capture;
    bool rejected = false;
    try {
        (void)knock::run_cli(arguments);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).starts_with("Help and info take no arguments");
    }
    require(rejected, "Extra guide arguments were not rejected before platform access");
    require(capture.text().empty(), "Invalid guide arguments printed a partial guide");
}

void presence_commands_use_readers_and_respect_wait_intent() {
    using knock::Json;
    const Json presence{{"state", "away"}, {"message", "Out for coffee."}, {"updated_at", 1000}};
    const Json query{{"ok", true}, {"presence", presence}};
    {
        FakeIpc ipc({query});
        Capture capture;
        require(knock::run_cli({L"presence"}) == 0 && Json::parse(capture.text()) == query,
                "presence did not return its read-only response");
        require(ipc_commands == std::vector<Json>{{{"op", "presence.get"}}},
                "presence command did not use the read-only operation");
    }
    const Json pending{{"id", "pending-1"}, {"kind", "input"}, {"status", "pending"}, {"lease_until", 0}};
    const Json created{{"ok", true}, {"request", pending}, {"presence", presence}};
    auto waiting = created;
    waiting["wait_reason"] = "presence";
    const std::vector<std::wstring> request{L"request", L"--chat", L"AUDIO-1", L"--message", L"May I test the interface?"};
    {
        FakeIpc ipc({created, waiting});
        Capture capture;
        require(knock::run_cli(request) == 7 && Json::parse(capture.text()) == waiting,
                "unavailable presence did not return the pending ID, guidance, and exit 7");
        require(ipc_commands.size() == 2 && ipc_commands[0]["op"] == "request.create" &&
                ipc_commands[1] == Json{{"op", "request.wait"}, {"id", "pending-1"},
                    {"wait_seconds", 1801}, {"stop_when_unavailable", true}},
                "ordinary request skipped the final decision check or omitted presence-aware waiting");
    }
    {
        FakeIpc ipc({created});
        Capture capture;
        auto detached = request;
        detached.push_back(L"--detach");
        require(knock::run_cli(detached) == 0 && Json::parse(capture.text()) == created,
                "detached request changed its pending response or success exit code");
        require(ipc_commands.size() == 1 && ipc_commands[0]["op"] == "request.create",
                "detached request unexpectedly waited");
    }
    {
        FakeIpc ipc({created});
        Capture capture;
        require(knock::run_cli({L"result", L"pending-1", L"--wait", L"300"}) == 7 &&
                Json::parse(capture.text()) == created, "explicit wait lost a pending response or presence");
        require(ipc_commands == std::vector<Json>{{{"op", "request.wait"}, {"id", "pending-1"}, {"wait_seconds", 300}}},
                "explicit result wait changed its time budget or enabled early presence return");
    }
    for (const auto& arguments : std::vector<std::vector<std::wstring>>{
            {L"presence", L"away"}, {L"presence", L"--state", L"away"}, {L"presence", L"--detach"}}) {
        FakeIpc ipc({});
        Capture capture;
        bool rejected = false;
        try { (void)knock::run_cli(arguments); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && ipc_commands.empty() && capture.text().empty(),
                "presence accepted editing arguments or contacted IPC before rejecting them");
    }
}

// Each existing command keeps its exact IPC operation, its unmodified JSON
// output and its exit code. New commands are checked the same way.
struct Contract {
    const char* name;
    std::vector<std::wstring> arguments;
    std::vector<knock::Json> commands;
    std::vector<knock::Json> responses;
    knock::Json printed;
    int exit_code;
};

void command_contracts_are_unchanged() {
    using knock::Json;
    const Json presence{{"state", "at_pc"}, {"message", ""}, {"updated_at", 5}};
    const auto request_record = [&](const char* status, std::int64_t lease_until = 0, const char* kind = "input") {
        return Json{{"id", "r1"}, {"chat", "AUDIO-1"}, {"message", "May I?"}, {"kind", kind},
                    {"status", status}, {"response", ""}, {"lease_seconds", 300}, {"lease_until", lease_until},
                    {"created_at", 1}, {"expires_at", 999999}};
    };
    const auto answered = [&](const char* status, std::int64_t lease_until = 0, const char* kind = "input") {
        return Json{{"ok", true}, {"request", request_record(status, lease_until, kind)}, {"presence", presence}};
    };
    const Json job{{"id", "j1"}, {"chat", "BUILD-1"}, {"title", "Build"}, {"status", "completed"}, {"exit_code", 0}};
    const Json task{{"id", "t1"}, {"title", "Ship"}, {"body", ""}, {"operator", "ME"}, {"project", ""}, {"status", "todo"}};
    const Json note{{"id", "n1"}, {"title", "Ideas"}, {"body", ""}, {"tags", Json::array()}};
    const Json done{{"id", "d1"}, {"chat", "UI-1"}, {"status", "done"}, {"summary", "All checks passed."},
                     {"report", "C:\\r.md"}, {"tasks_completed", Json::array({"t1"})}};
    const auto page = [](const char* collection, const char* key, Json item) {
        return std::pair{Json{{"op", "list"}, {"collection", collection}, {"offset", 0}, {"limit", 50},
                              {"summaries", std::string_view(collection) == "jobs"}},
                         std::pair{Json{{"ok", true}, {"items", Json::array({item})}, {"total", 1}, {"next_offset", nullptr}},
                                   Json{{"ok", true}, {key, Json::array({item})}, {"total", 1}, {"next_offset", nullptr}}}};
    };
    const Json agent{{"id", "a1"}, {"chat", "UI-1"}, {"title", "Redesign the window"}, {"status", "working"},
                     {"status_text", ""}, {"report", ""}, {"started_at", 5}, {"updated_at", 5}};
    const Json failure{{"ok", false}, {"error", "not_found: zz"}};
    const Json status{{"ok", true}, {"requests_pending", 0}, {"jobs_running", 0}, {"tasks_todo", 1},
                      {"notes", 0}, {"lease", nullptr}, {"presence", presence}};
    const auto [jobs_command, jobs_io] = page("jobs", "jobs", job);
    const auto [tasks_command, tasks_io] = page("tasks", "tasks", task);
    const auto [notes_command, notes_io] = page("notes", "notes", note);
    const auto [inbox_command, inbox_io] = page("inbox", "inbox", done);
    auto paged_jobs = jobs_command;
    paged_jobs["offset"] = 50;
    paged_jobs["limit"] = 10;

    const std::vector<Contract> contracts{
        {"status", {L"status"}, {{{"op", "status"}}}, {status}, status, 0},
        {"presence", {L"presence"}, {{{"op", "presence.get"}}}, {{{"ok", true}, {"presence", presence}}},
            {{"ok", true}, {"presence", presence}}, 0},
        {"request approved", {L"request", L"--chat", L"AUDIO-1", L"--message", L"May I?", L"--timeout", L"60", L"--lease", L"120"},
            {{{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "May I?"}, {"kind", "input"},
              {"timeout_seconds", 60}, {"lease_seconds", 120}},
             {{"op", "request.wait"}, {"id", "r1"}, {"wait_seconds", 61}, {"stop_when_unavailable", true}}},
            {answered("pending"), answered("approved", 5000)}, answered("approved", 5000), 0},
        {"request approved after lease end", {L"request", L"--chat", L"AUDIO-1", L"--message", L"May I?"},
            {{{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "May I?"}, {"kind", "input"},
              {"timeout_seconds", 1800}, {"lease_seconds", 300}},
             {{"op", "request.wait"}, {"id", "r1"}, {"wait_seconds", 1801}, {"stop_when_unavailable", true}}},
            {answered("pending"), answered("approved", 500)}, answered("approved", 500), 4},
        {"question denied", {L"request", L"--chat", L"AUDIO-1", L"--message", L"May I?", L"--kind", L"question"},
            {{{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "May I?"}, {"kind", "question"},
              {"timeout_seconds", 1800}, {"lease_seconds", 300}},
             {{"op", "request.wait"}, {"id", "r1"}, {"wait_seconds", 1801}, {"stop_when_unavailable", true}}},
            {answered("pending", 0, "question"), answered("denied", 0, "question")}, answered("denied", 0, "question"), 2},
        {"result custom", {L"result", L"r1"}, {{{"op", "request.result"}, {"id", "r1"}}},
            {answered("custom")}, answered("custom"), 3},
        {"result expired", {L"result", L"r1"}, {{{"op", "request.result"}, {"id", "r1"}}},
            {answered("expired")}, answered("expired"), 4},
        {"result cancelled", {L"result", L"r1"}, {{{"op", "request.result"}, {"id", "r1"}}},
            {answered("cancelled")}, answered("cancelled"), 6},
        {"result error", {L"result", L"zz"}, {{{"op", "request.result"}, {"id", "zz"}}}, {failure}, failure, 5},
        {"release", {L"release", L"r1"}, {{{"op", "request.release"}, {"id", "r1"}}},
            {answered("released")}, answered("released"), 0},
        {"cancel", {L"cancel", L"r1"}, {{{"op", "request.cancel"}, {"id", "r1"}}},
            {answered("cancelled")}, answered("cancelled"), 0},
        {"notify", {L"notify", L"--chat", L"BUILD-1", L"--message", L"Done", L"--resume-text", L"Continue"},
            {{{"op", "notify"}, {"chat", "BUILD-1"}, {"message", "Done"}, {"resume_text", "Continue"}}},
            {{{"ok", true}, {"job", job}}}, {{"ok", true}, {"job", job}}, 0},
        {"job", {L"job", L"j1"}, {{{"op", "job.result"}, {"id", "j1"}}},
            {{{"ok", true}, {"job", job}}}, {{"ok", true}, {"job", job}}, 0},
        {"jobs", {L"jobs"}, {jobs_command}, {jobs_io.first}, jobs_io.second, 0},
        {"jobs paged", {L"jobs", L"--offset", L"50", L"--limit", L"10"}, {paged_jobs}, {jobs_io.first}, jobs_io.second, 0},
        {"tasks list", {L"tasks", L"list"}, {tasks_command}, {tasks_io.first}, tasks_io.second, 0},
        {"tasks add", {L"tasks", L"add", L"--title", L"Ship", L"--operator", L"UI-1", L"--project", L"CURRENT"},
            {{{"op", "task.add"}, {"title", "Ship"}, {"operator", "UI-1"}, {"project", "CURRENT"}}},
            {{{"ok", true}, {"task", task}}}, {{"ok", true}, {"task", task}}, 0},
        {"tasks update", {L"tasks", L"update", L"t1", L"--status", L"done"},
            {{{"op", "task.update"}, {"id", "t1"}, {"status", "done"}}},
            {{{"ok", true}, {"task", task}}}, {{"ok", true}, {"task", task}}, 0},
        {"tasks remove", {L"tasks", L"remove", L"t1"}, {{{"op", "task.delete"}, {"id", "t1"}}},
            {{{"ok", true}}}, {{"ok", true}}, 0},
        {"notes list", {L"notes", L"list"}, {notes_command}, {notes_io.first}, notes_io.second, 0},
        {"notes get", {L"notes", L"get", L"n1", L"--offset", L"2", L"--limit", L"5"},
            {{{"op", "note.get"}, {"id", "n1"}, {"offset", 2}, {"limit", 5}}},
            {{{"ok", true}, {"note", note}}}, {{"ok", true}, {"note", note}}, 0},
        {"notes add", {L"notes", L"add", L"--title", L"Ideas", L"--tag", L"A", L"--tag", L"B"},
            {{{"op", "note.add"}, {"title", "Ideas"}, {"tags", Json::array({"A", "B"})}}},
            {{{"ok", true}, {"note", note}}}, {{"ok", true}, {"note", note}}, 0},
        {"notes update", {L"notes", L"update", L"n1", L"--clear-tags"},
            {{{"op", "note.update"}, {"id", "n1"}, {"tags", Json::array()}}},
            {{{"ok", true}, {"note", note}}}, {{"ok", true}, {"note", note}}, 0},
        {"notes append", {L"notes", L"append", L"n1", L"--body", L"More"},
            {{{"op", "note.append"}, {"id", "n1"}, {"text", "More"}}},
            {{{"ok", true}, {"message_count", 1}}}, {{"ok", true}, {"message_count", 1}}, 0},
        {"notes remove", {L"notes", L"remove", L"n1"}, {{{"op", "note.delete"}, {"id", "n1"}}},
            {{{"ok", true}, {"note", note}}}, {{"ok", true}, {"note", note}}, 0},
        {"done", {L"done", L"--chat", L"UI-1", L"--summary", L"All checks passed."},
            {{{"op", "done"}, {"chat", "UI-1"}, {"summary", "All checks passed."}, {"report", ""}}},
            {{{"ok", true}, {"done", done}}}, {{"ok", true}, {"done", done}}, 0},
        {"done with task", {L"done", L"--chat", L"UI-1", L"--summary", L"All checks passed.", L"--task", L"t1"},
            {{{"op", "done"}, {"chat", "UI-1"}, {"summary", "All checks passed."}, {"report", ""}, {"task", "t1"}}},
            {{{"ok", true}, {"done", done}}}, {{"ok", true}, {"done", done}}, 0},
        {"inbox", {L"inbox"}, {inbox_command}, {inbox_io.first}, inbox_io.second, 0},
        {"start", {L"start", L"--chat", L"UI-1", L"--title", L"Redesign the window"},
            {{{"op", "agent.start"}, {"chat", "UI-1"}, {"title", "Redesign the window"}, {"report", ""}}},
            {{{"ok", true}, {"agent", agent}, {"presence", presence}}}, {{"ok", true}, {"agent", agent}, {"presence", presence}}, 0},
        {"start refused", {L"start", L"--chat", L"UI-1", L"--title", L"Redesign the window"},
            {{{"op", "agent.start"}, {"chat", "UI-1"}, {"title", "Redesign the window"}, {"report", ""}}},
            {failure}, failure, 5},
        {"update", {L"update", L"--chat", L"UI-1", L"--status", L"Running tests"},
            {{{"op", "agent.update"}, {"chat", "UI-1"}, {"status", "Running tests"}}},
            {{{"ok", true}, {"agent", agent}, {"presence", presence}}}, {{"ok", true}, {"agent", agent}, {"presence", presence}}, 0},
    };
    for (const auto& contract : contracts) {
        FakeIpc ipc(contract.responses);
        Capture capture;
        const int code = knock::run_cli(contract.arguments);
        if (code != contract.exit_code)
            throw std::runtime_error(std::string(contract.name) + ": exit " + std::to_string(code));
        if (ipc_commands != contract.commands)
            throw std::runtime_error(std::string(contract.name) + ": sent " + Json(ipc_commands).dump());
        if (capture.text() != contract.printed.dump(-1, ' ', true) + "\n")
            throw std::runtime_error(std::string(contract.name) + ": printed " + capture.text());
    }

    {   // A report path is stored absolute so it can be pasted anywhere.
        FakeIpc ipc({{{"ok", true}, {"done", done}}});
        Capture capture;
        require(knock::run_cli({L"done", L"--chat", L"UI-1", L"--summary", L"Done.", L"--report", L"report.md"}) == 0,
                "done with a report failed");
        const auto expected = (std::filesystem::current_path() / "report.md").string();
        require(ipc_commands.size() == 1 && ipc_commands[0]["report"] == expected,
                "done did not send an absolute report path");
    }
    {   // start stores the report as an absolute path too.
        FakeIpc ipc({{{"ok", true}, {"agent", agent}}});
        Capture capture;
        require(knock::run_cli({L"start", L"--chat", L"UI-1", L"--title", L"Work", L"--report", L"report.md"}) == 0,
                "start with a report failed");
        const auto expected = (std::filesystem::current_path() / "report.md").string();
        require(ipc_commands.size() == 1 && ipc_commands[0]["report"] == expected,
                "start did not send an absolute report path");
    }
    {
        FakeIpc ipc({{{"ok", true}}});
        Capture capture;
        require(knock::run_cli({L"open"}) == 0 && capture.text() == "{\"ok\":true}\n" && ipc_commands.empty(),
                "open changed its output");
    }
    {
        FakeIpc ipc({});
        started_job.reset();
        Capture capture;
        require(knock::run_cli({L"run", L"--chat", L"BUILD-1", L"--title", L"Build", L"--cwd", L".", L"--",
                                L"cmake.exe", L"--build", L"build"}) == 0, "run failed");
        require(started_job && started_job->chat == "BUILD-1" && started_job->title == "Build" &&
                started_job->command == std::vector<std::wstring>{L"cmake.exe", L"--build", L"build"} &&
                Json::parse(capture.text()) == Json{{"ok", true}, {"job", {{"id", "j1"}, {"status", "running"}}}},
                "run changed its options or output");
    }
    for (const auto& arguments : std::vector<std::vector<std::wstring>>{
            {L"done", L"--summary", L"x"}, {L"done", L"--chat", L"UI-1"}, {L"done", L"--chat", L"UI-1", L"--summary", L""},
            {L"done", L"--chat", L"UI-1", L"--summary", L"x", L"--summary-file", L"y"},
            {L"done", L"--chat", L"UI-1", L"--summary", L"x", L"--detach"},
            {L"done", L"--chat", L"UI-1", L"--summary", L"x", L"--report", L""},
            {L"inbox", L"extra"}, {L"inbox", L"--limit", L"0"}, {L"inbox", L"--chat", L"UI-1"},
            {L"start"}, {L"start", L"--chat", L"UI-1"}, {L"start", L"--title", L"x"}, {L"start", L"--chat", L"UI-1", L"--title", L""},
            {L"start", L"--chat", L"UI-1", L"--title", L"x", L"--report", L""},
            {L"start", L"--chat", L"UI-1", L"--title", L"x", L"--detach"},
            {L"start", L"--chat", L"UI-1", L"--title", std::wstring(161, L'x')},
            {L"start", L"--chat", L"UI-1", L"--title", L"x", L"--status", L"y"}, {L"start", L"UI-1"},
            {L"update"}, {L"update", L"--chat", L"UI-1"}, {L"update", L"--status", L"x"},
            {L"update", L"--chat", L"UI-1", L"--status", L""}, {L"update", L"--chat", L"UI-1", L"--status", std::wstring(161, L'x')},
            {L"update", L"--chat", L"UI-1", L"--status", L"x", L"--title", L"y"},
            {L"quiet"}, {L"quiet", L"--chat", L"RUN-1"}, {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5"},
            {L"quiet", L"--chat", L"RUN-1", L"--reason", L"x"}, {L"quiet", L"--minutes", L"5", L"--reason", L"x"},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"0", L"--reason", L"x"},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"241", L"--reason", L"x"},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"ten", L"--reason", L"x"},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5", L"--reason", L""},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5", L"--reason", std::wstring(161, L'x')},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5", L"--reason", L"x", L"--timeout", L"0"},
            {L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5", L"--reason", L"x", L"--lease", L"5"},
            {L"quiet", L"--release", L"--status"}, {L"quiet", L"--release", L"--minutes", L"5"},
            {L"quiet", L"--release", L"--detach"}, {L"quiet", L"--status", L"--chat", L"RUN-1"}, {L"quiet", L"--status", L"extra"}}) {
        FakeIpc ipc({});
        Capture capture;
        bool rejected = false;
        try { (void)knock::run_cli(arguments); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && ipc_commands.empty() && capture.text().empty(),
                "invalid done, inbox, start, update or quiet arguments reached IPC");
    }
}

void quiet_commands_wait_and_report_by_exit_code() {
    using knock::Json;
    const Json closed{{"open", false}, {"waiting", 0}};
    const Json open{{"id", "w1"}, {"chat", "RUN-1"}, {"reason", "Timing"}, {"minutes", 5}, {"status", "open"}, {"until", 900000}};
    auto waiting = open;
    waiting["status"] = "waiting";
    auto ended = open;
    ended["status"] = "ended";
    ended["end_reason"] = "timed_out";
    const Json opened{{"ok", true}, {"quiet", open}, {"quiet_window", closed}};
    const Json queued{{"ok", true}, {"quiet", waiting}, {"queue", {{"position", 1}, {"expected_wait_seconds", 300}}}, {"quiet_window", closed}};
    const Json gave_up{{"ok", true}, {"quiet", ended}, {"quiet_window", closed}};
    const Json nothing{{"ok", true}, {"quiet", nullptr}, {"quiet_window", closed}};
    const Json failure{{"ok", false}, {"error", "boom"}};
    const Json status{{"ok", true}, {"quiet_window", closed}, {"waiting", Json::array()}};
    const std::vector<std::wstring> ask{L"quiet", L"--chat", L"RUN-1", L"--minutes", L"5", L"--reason", L"Timing"};
    const Json create{{"op", "quiet.open"}, {"chat", "RUN-1"}, {"reason", "Timing"}, {"minutes", 5}, {"timeout_seconds", 1800}};
    const Json wait{{"op", "quiet.wait"}, {"id", "w1"}, {"wait_seconds", 1801}};
    auto detached = ask;
    detached.push_back(L"--detach");
    auto with_timeout = ask;
    with_timeout.insert(with_timeout.end(), {L"--timeout", L"60"});
    auto create_60 = create;
    create_60["timeout_seconds"] = 60;
    const std::vector<Contract> contracts{
        {"quiet opens at once", ask, {create}, {opened}, opened, 0},
        {"quiet refused", ask, {create}, {failure}, failure, 5},
        {"quiet waits its turn", ask, {create, wait}, {queued, opened}, opened, 0},
        {"quiet still waiting at the end of the budget", ask, {create, wait}, {queued, queued}, queued, 7},
        {"quiet gave up", ask, {create, wait}, {queued, gave_up}, gave_up, 4},
        {"quiet detached", detached, {create}, {queued}, queued, 0},
        {"quiet timeout", with_timeout, {create_60, {{"op", "quiet.wait"}, {"id", "w1"}, {"wait_seconds", 61}}}, {queued, opened}, opened, 0},
        {"quiet release", {L"quiet", L"--release"}, {{{"op", "quiet.release"}}}, {gave_up}, gave_up, 0},
        {"quiet release of nothing", {L"quiet", L"--release"}, {{{"op", "quiet.release"}}}, {nothing}, nothing, 0},
        {"quiet release by chat", {L"quiet", L"--release", L"--chat", L"RUN-1"}, {{{"op", "quiet.release"}, {"chat", "RUN-1"}}}, {nothing}, nothing, 0},
        {"quiet status", {L"quiet", L"--status"}, {{{"op", "quiet.status"}}}, {status}, status, 0},
    };
    for (const auto& contract : contracts) {
        FakeIpc ipc(contract.responses);
        Capture capture;
        const int code = knock::run_cli(contract.arguments);
        if (code != contract.exit_code)
            throw std::runtime_error(std::string(contract.name) + ": exit " + std::to_string(code));
        if (ipc_commands != contract.commands)
            throw std::runtime_error(std::string(contract.name) + ": sent " + Json(ipc_commands).dump());
        if (capture.text() != contract.printed.dump(-1, ' ', true) + "\n")
            throw std::runtime_error(std::string(contract.name) + ": printed " + capture.text());
    }
    {   // A wait that loses Knock keeps the ID, so the same command can pick it up.
        FakeIpc ipc({queued, failure});
        Capture capture;
        require(knock::run_cli(ask) == 5 && Json::parse(capture.text()).at("quiet_id") == "w1", "A lost wait dropped its ID");
    }
}

void info_lists_who_is_on_this_pc() {
    using knock::Json;
    const auto guide_only = std::string(knock::agent_info);
    {   // Knock closed: the guide still prints, nothing starts, and it says what it can't know.
        FakeIpc ipc({{{"ok", false}, {"error", "Knock is not responding"}}});
        const auto text = invoke({L"info"});
        require(text.starts_with(guide_only) && text.find("Knock is not running") != std::string::npos, "info failed without the tray");
        require(ipc_commands == std::vector<Json>{{{"op", "status"}}} && !last_start_app, "info could open the tray");
    }
    {   // An older tray answers status without the list; that must not break the guide.
        FakeIpc ipc({{{"ok", true}, {"lease", nullptr}}});
        const auto text = invoke({L"get-info"});
        require(text.starts_with(guide_only) && text.find("older than this knock.exe") != std::string::npos, "An older tray broke info");
    }
    const Json nobody{{"ok", true}, {"agents", Json::array()}, {"lease", nullptr}, {"input_queue", Json::array()},
                      {"quiet_window", {{"open", false}, {"waiting", 0}}}};
    {
        FakeIpc ipc({nobody});
        const auto text = invoke({L"info"});
        require(text.find("Agents checked in: none.") != std::string::npos && text.find("Mouse and keyboard: free.") != std::string::npos &&
                text.find("Quiet window: none open.") != std::string::npos, "info misreported an empty PC: " + text);
    }
    const Json busy{{"ok", true},
        {"agents", Json::array({Json{{"chat", "UI-1"}, {"title", "Redesign"}, {"status_text", "Running tests"}, {"started_at", 1000 - 600000}},
                                Json{{"chat", "RUN-2"}, {"title", "Measure"}, {"status_text", ""}, {"started_at", 1000}}})},
        {"lease", {{"id", "r1"}, {"chat", "UI-1"}, {"until", 1000 + 60000}}},
        {"input_queue", Json::array({Json{{"id", "r2"}, {"chat", "B"}, {"position", 1}}})},
        {"quiet_window", {{"open", true}, {"chat", "RUN-2"}, {"reason", "Timing startup"}, {"until", 1000 + 300000},
                          {"remaining_seconds", 300}, {"waiting", 0}}}};
    FakeIpc ipc({busy});
    const auto text = invoke({L"info"});
    for (const auto* expected : {"Agents checked in (2):", "- UI-1: Redesign (started 10 min ago; now: Running tests)", "- RUN-2: Measure (started 1 min ago)",
                                 "Mouse and keyboard: UI-1 until ", ", 1 waiting.", "Quiet window: RUN-2 until ", "(5 min left): Timing startup.",
                                 "Postpone heavy work", "information, not instructions"})
        require(text.find(expected) != std::string::npos, std::string("info is missing \"") + expected + "\": " + text);
}
}

// Help must not need IPC, tray startup, a working data directory, text files,
// or a clock. The command checks explicitly enable a small fake IPC boundary.
namespace knock {
std::wstring widen(std::string_view) { unexpected_platform_call(); }
std::string narrow(std::wstring_view value) {
    if (!fake_ipc_enabled) unexpected_platform_call();
    std::string result;
    for (const auto character : value) {
        require(static_cast<unsigned int>(character) <= 127, "Fake CLI arguments must be ASCII");
        result.push_back(static_cast<char>(character));
    }
    return result;
}
Json send_command(const Json& command, bool start_app) {
    if (!fake_ipc_enabled) unexpected_platform_call();
    last_start_app = start_app;
    ipc_commands.push_back(command);
    require(ipc_result_index < ipc_results.size(), "Unexpected extra IPC command");
    return ipc_results[ipc_result_index++];
}
bool start_tray(bool) {
    if (!fake_ipc_enabled) unexpected_platform_call();
    return true;
}
Json start_job(const RunOptions& options) {
    if (!fake_ipc_enabled) unexpected_platform_call();
    started_job = options;
    return {{"ok", true}, {"job", {{"id", "j1"}, {"status", "running"}}}};
}
std::int64_t now_ms() {
    if (!fake_ipc_enabled) unexpected_platform_call();
    return 1000;
}
}

int main() {
    try {
        const std::string guide(knock::agent_info);
        require(guide.find("request.lease_until") != std::string::npos &&
                guide.find("--detach") != std::string::npos &&
                guide.find("--wait 300") != std::string::npos &&
                guide.find("does not automatically wake a CLI coding agent") != std::string::npos &&
                guide.find("Do not poll") != std::string::npos &&
                guide.find("next_offset") != std::string::npos &&
                guide.find("knock start --chat") != std::string::npos && guide.find("knock update --chat") != std::string::npos &&
                guide.find("knock quiet --chat") != std::string::npos && guide.find("knock quiet --release") != std::string::npos &&
                guide.find("queue.position") != std::string::npos,
                "Guide is missing a required permission or efficient-waiting rule");
        require(guide.size() < 16 * 1024, "Agent guide grew beyond a small one-call response");
        std::cout << "PASS info and alias include permission, waiting, and bounded-output guidance\n";

        info_lists_who_is_on_this_pc();
        std::cout << "PASS info lists who is on this PC and never opens the tray\n";

        const auto help = invoke({L"help"});
        for (const auto& arguments : std::vector<std::vector<std::wstring>>{{}, {L"--help"}, {L"-h"}})
            require(invoke(arguments) == help, "Help aliases differ");
        require(help.find("knock info") != std::string::npos && help.find("knock presence") != std::string::npos &&
                help.find("knock done") != std::string::npos && help.find("knock inbox") != std::string::npos &&
                help.find("knock start") != std::string::npos && help.find("knock update") != std::string::npos &&
                help.find("knock quiet --release") != std::string::npos && help.find("knock quiet --status") != std::string::npos,
                "Short help does not expose info and presence");
        std::cout << "PASS short help points to the embedded agent guide\n";

        for (const auto* action : {L"info", L"get-info", L"help", L"--help", L"-h"}) {
            rejects({action, L"unexpected"});
            rejects({action, L"--body-file", L"missing-private-file.txt"});
            rejects({action, L"--"});
        }
        require(platform_calls == 0, "Help or info contacted the application");
        std::cout << "PASS extra arguments fail without tray, IPC, or platform access\n";

        presence_commands_use_readers_and_respect_wait_intent();
        require(platform_calls == 0, "CLI routing checks crossed an unexpected platform boundary");
        std::cout << "PASS presence, pending exit codes, and explicit wait intent\n";

        command_contracts_are_unchanged();
        require(platform_calls == 0, "Command contract checks crossed an unexpected platform boundary");
        std::cout << "PASS every command keeps its operation, JSON output, and exit code; done and inbox\n";

        quiet_commands_wait_and_report_by_exit_code();
        require(platform_calls == 0, "Quiet command checks crossed an unexpected platform boundary");
        std::cout << "PASS quiet opens, waits its turn, releases, and reports by exit code\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
