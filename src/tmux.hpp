#pragma once

#include "process.hpp"
#include "result.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace jjmcp {

struct PaneInfo {
    std::string pane_id;
    std::string session_name;
    std::string window_index;
    std::string pane_index;
    std::string current_command;
    std::string title;
    std::string active;
    std::string dead;
    std::string current_path;

    [[nodiscard]] bool alive() const { return dead != "1" && !pane_id.empty(); }
};

class Tmux {
public:
    explicit Tmux(ProcessRunner runner = {});

    Result<std::string> list_all() const;
    Result<PaneInfo> pane_info(const std::string& target) const;
    Result<std::string> capture_pane(const std::string& target, int lines) const;
    Result<void> send_text(const std::string& target, const std::string& text, const std::string& buffer_name) const;
    Result<void> send_ctrl_c(const std::string& target) const;
    // Send a single tmux key spec (e.g. "BSpace", "Enter", "C-c") to the bound pane.
    Result<void> send_key(const std::string& target, const std::string& key) const;

private:
    Result<ProcessResult> run_tmux(
        const std::vector<std::string>& args,
        std::string stdin_data = {},
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) const;

    ProcessRunner runner_;
};

std::vector<std::string> make_tmux_argv(const std::vector<std::string>& args);

} // namespace jjmcp
