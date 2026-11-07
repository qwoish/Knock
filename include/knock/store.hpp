#pragma once

#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace knock {
using Json = nlohmann::json;
using ChangeCallback = std::function<void(const Json&)>;

// The tray process owns this store. Only direct GUI calls set trusted_ui=true.
class Store {
public:
    explicit Store(std::filesystem::path directory);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void load();
    Json handle(const Json& command, bool trusted_ui = false);
    Json snapshot();
    void set_changed(ChangeCallback callback);
    void tick();
    void stop();
    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    Json state_;
    std::mutex mutex_;
    std::condition_variable changed_;
    ChangeCallback callback_;
    bool stopped_ = false;
    bool loaded_ = false;

    // A queue moved inside expiry; delivered once the lock is released.
    std::vector<Json> line_events_;

    void save_locked();
    bool expire_locked();
};

std::int64_t now_ms();
std::string make_id();

// Where a pending input request stands in the line for the mouse and keyboard, or
// null when nothing is ahead of it. expected_wait_seconds adds up the lease being used
// and the leases asked for ahead; it does not count the time the person takes to answer.
Json input_queue_place(const Json& state, const Json& request, std::int64_t now);
// True while this request must wait its turn: the person's YES is not offered yet.
bool input_request_queued(const Json& state, const Json& request);
// Whether a timed run is open, whose, until when, and how many wait behind it.
Json quiet_window_view(const Json& state, std::int64_t now);
}
