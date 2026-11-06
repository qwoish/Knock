#pragma once

#include "knock/store.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace knock {
// IsWindowVisible, except that it does not depend on the desktop window, which is
// never shown in a private window station (see tests/isolated_run.cpp).
inline bool window_shown(HWND window) {
    if (!window) return false;
    for (; window; window = GetParent(window))
        if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)) return false;
    return true;
}
std::wstring widen(std::string_view value);
std::string narrow(std::wstring_view value);
std::filesystem::path executable_path();
std::filesystem::path data_directory();
std::wstring user_identity();
// A copy pointed at its own folder with KNOCK_DATA_DIR is a separate instance:
// its pipe, tray window class and app lock are all derived from that folder, so
// it can never reach a Knock using the default data folder. Empty by default.
std::wstring instance_tag();
std::wstring pipe_name();
std::wstring tray_window_class();
std::wstring app_lock_name();
std::wstring quote_argument(std::wstring_view argument);
std::wstring command_line(const std::vector<std::wstring>& arguments);
Json send_command(const Json& command, bool start_app = true);
bool start_tray(bool show = false);
class PipeServer {
public:
    explicit PipeServer(Store& store);
    ~PipeServer();
    void start();
    void stop();
private:
    struct Impl;
    Impl* impl_;
};
int run_cli(const std::vector<std::wstring>& arguments);
int run_worker(const std::vector<std::wstring>& arguments);
}
