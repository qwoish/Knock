// One-off: writes a state.json with the store at commit 72f4709, the source of the
// 0.7.0 build, using only its own operations. Never reads any real Knock data.
#include "knock/store.hpp"
#include <fstream>
#include <iostream>

using knock::Json;
static Json ok(Json r) { if (!r.value("ok", false)) { std::cerr << r.dump() << "\n"; std::exit(1); } return r; }

int main(int, char** argv) {
    std::filesystem::path dir = argv[1];
    knock::Store s(dir);
    s.load();
    auto ui = [&](Json c) { return ok(s.handle(c, true)); };
    auto cli = [&](Json c) { return ok(s.handle(c, false)); };

    // Requests in every lasting state.
    auto pending = cli({{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "May I use the mouse and keyboard? \xE2\x80\x94 caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"}, {"timeout_seconds", 86400}, {"lease_seconds", 300}})["request"];
    auto question = cli({{"op", "request.create"}, {"chat", "AUDIO-2"}, {"kind", "question"}, {"message", "Keep the old mixer?"}, {"timeout_seconds", 86400}})["request"];
    auto denied = cli({{"op", "request.create"}, {"chat", "EDIT-1"}, {"message", "Input for drag tests?"}, {"timeout_seconds", 600}, {"lease_seconds", 120}})["request"];
    ui({{"op", "request.resolve"}, {"id", denied["id"]}, {"decision", "denied"}});
    auto custom = cli({{"op", "request.create"}, {"chat", "EDIT-2"}, {"message", "Input for scroll tests?"}, {"timeout_seconds", 600}})["request"];
    ui({{"op", "request.resolve"}, {"id", custom["id"]}, {"decision", "custom"}, {"text", "Not now. Continue with headless tests."}});
    auto cancelled = cli({{"op", "request.create"}, {"chat", "EDIT-3"}, {"message", "Never mind."}, {"timeout_seconds", 600}})["request"];
    cli({{"op", "request.cancel"}, {"id", cancelled["id"]}});
    auto released = cli({{"op", "request.create"}, {"chat", "EDIT-4"}, {"message", "Input for one check?"}, {"timeout_seconds", 600}, {"lease_seconds", 60}})["request"];
    ui({{"op", "request.resolve"}, {"id", released["id"]}, {"decision", "approved"}});
    cli({{"op", "request.release"}, {"id", released["id"]}});
    cli({{"op", "item.dismiss"}, {"kind", "request"}, {"id", denied["id"]}});

    // Jobs: running, finished, failed, notice.
    auto running = cli({{"op", "job.create"}, {"chat", "BUILD-1"}, {"title", "Release build"}, {"command", "cmake --build build"}, {"cwd", "C:\\Projects\\Current"}})["job"];
    cli({{"op", "job.started"}, {"id", running["id"]}, {"worker_pid", 4242}});
    auto finished = cli({{"op", "job.create"}, {"chat", "BUILD-2"}, {"title", "Tests"}, {"command", "ctest"}, {"cwd", "C:\\Projects\\Current"}})["job"];
    cli({{"op", "job.finish"}, {"id", finished["id"]}, {"exit_code", 0}, {"output_tail", "100% tests passed"}, {"log_path", "C:\\logs\\tests.log"}});
    auto failed = cli({{"op", "job.create"}, {"chat", "BUILD-3"}, {"title", "Lint"}, {"command", "lint"}, {"cwd", "C:\\Projects\\Current"}})["job"];
    cli({{"op", "job.finish"}, {"id", failed["id"]}, {"exit_code", 3}, {"output_tail", "3 problems"}, {"log_path", "C:\\logs\\lint.log"}});
    cli({{"op", "notify"}, {"chat", "RENDER-1"}, {"message", "Render finished."}, {"resume_text", "@RENDER-1: continue"}});

    // Tasks in each status.
    auto t1 = cli({{"op", "task.add"}, {"title", "AUDIO-1: mixer tests"}, {"operator", "AUDIO-1"}, {"project", "CURRENT"}})["task"];
    auto t2 = cli({{"op", "task.add"}, {"title", "Licence question"}, {"body", "Ask about the fonts."}})["task"];
    cli({{"op", "task.update"}, {"id", t2["id"]}, {"status", "in_progress"}});
    cli({{"op", "task.add"}, {"title", "UI-1: polish"}, {"operator", "UI-1"}});

    // Notes: tags, messages, an edited entry with a client ID, images, Trash.
    auto n1 = cli({{"op", "note.add"}, {"title", "Project ideas"}, {"body", "A description."}, {"tags", {"Ideas", "\xE6\x97\x85\xE8\xA1\x8C"}}})["note"];
    auto m1 = ui({{"op", "note.append"}, {"id", n1["id"]}, {"text", "First thought."}, {"images", Json::array()}, {"client_id", "0123456789abcdef0123456789abcdef"}})["message"];
    cli({{"op", "note.append"}, {"id", n1["id"]}, {"text", "Second thought\nwith a line break."}});
    ui({{"op", "note.message.edit"}, {"id", n1["id"]}, {"message_id", m1["id"]}, {"expected_revision", 0}, {"text", "First thought, edited."}, {"images", Json::array()}});
    cli({{"op", "note.add"}, {"title", "Empty note"}});
    auto gone = cli({{"op", "note.add"}, {"title", "Deleted note"}})["note"];
    cli({{"op", "note.append"}, {"id", gone["id"]}, {"text", "Kept in Trash."}});
    cli({{"op", "note.delete"}, {"id", gone["id"]}});

    // Finished jobs, one archived, one that completed its task.
    cli({{"op", "done"}, {"chat", "UI-1"}, {"summary", "Polish done.\nAll checks passed."}, {"report", "C:\\Projects\\r.md"}});
    auto archived = cli({{"op", "done"}, {"chat", "SITE-1"}, {"summary", "Old job."}, {"report", ""}})["done"];
    cli({{"op", "item.dismiss"}, {"kind", "inbox"}, {"id", archived["id"]}});

    // Preferences and presence as the GUI sets them.
    ui({{"op", "preferences.update"}, {"saved_replies", {"Later, please.", "Continue with headless tests."}}, {"trash_retention_days", 45}});
    ui({{"op", "presence.update"}, {"state", "away"}, {"message", "Out for coffee."}});
    s.stop();

    // Keep it loadable for decades: pending requests would otherwise expire, and
    // the Trash entry would age out, which are real rewrites unrelated to this test.
    auto state = Json::parse(std::ifstream(dir / "state.json", std::ios::binary));
    for (auto& r : state["requests"]) if (r["status"] == "pending") r["expires_at"] = 4102444800000LL;
    for (auto& n : state["trash"]) n["deleted_at"] = 4102444800000LL;
    std::ofstream(dir / "state.json", std::ios::binary | std::ios::trunc) << state.dump(2) << '\n';
    std::cout << "ok\n";
}
