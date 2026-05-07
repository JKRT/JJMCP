#include "tmux.hpp"

#include <sstream>

namespace jjmcp {
namespace {

std::vector<std::string> split_tabs(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == '\t') {
            fields.push_back(current);
            current.clear();
        } else if (c != '\r' && c != '\n') {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

std::string command_failure(const std::string& label, const ProcessResult& result)
{
    std::string message = label + " failed: " + describe_process_result(result);
    if (!result.stderr_text.empty()) {
        message += ": " + result.stderr_text;
    } else if (!result.stdout_text.empty()) {
        message += ": " + result.stdout_text;
    }
    return message;
}

} // namespace

Tmux::Tmux(ProcessRunner runner) : runner_(std::move(runner)) {}

std::vector<std::string> make_tmux_argv(const std::vector<std::string>& args)
{
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back("tmux");
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

Result<ProcessResult> Tmux::run_tmux(
    const std::vector<std::string>& args,
    std::string stdin_data,
    std::chrono::milliseconds timeout) const
{
    RunSpec spec;
    spec.argv = make_tmux_argv(args);
    spec.stdin_data = std::move(stdin_data);
    spec.timeout = timeout;
    return runner_.run(spec);
}

Result<std::string> Tmux::list_all() const
{
    const std::string session_fmt = "#{session_name}\t#{session_id}\t#{session_windows}\t#{session_attached}";
    const std::string window_fmt = "#{session_name}\t#{window_index}\t#{window_id}\t#{window_name}\t#{window_active}\t#{window_panes}";
    const std::string pane_fmt = "#{session_name}\t#{window_index}\t#{window_name}\t#{pane_index}\t#{pane_id}\t#{pane_active}\t#{pane_current_command}\t#{pane_title}\t#{pane_current_path}";

    const auto sessions = run_tmux({"list-sessions", "-F", session_fmt});
    if (!sessions) {
        return Result<std::string>::failure(sessions.error());
    }
    if (sessions.value().exit_code != 0) {
        return Result<std::string>::failure(command_failure("tmux list-sessions", sessions.value()));
    }

    const auto windows = run_tmux({"list-windows", "-a", "-F", window_fmt});
    if (!windows) {
        return Result<std::string>::failure(windows.error());
    }
    if (windows.value().exit_code != 0) {
        return Result<std::string>::failure(command_failure("tmux list-windows", windows.value()));
    }

    const auto panes = run_tmux({"list-panes", "-a", "-F", pane_fmt});
    if (!panes) {
        return Result<std::string>::failure(panes.error());
    }
    if (panes.value().exit_code != 0) {
        return Result<std::string>::failure(command_failure("tmux list-panes", panes.value()));
    }

    std::string out;
    out += "Sessions: session_name\\tsession_id\\twindows\\tattached\n";
    out += sessions.value().stdout_text;
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out += "\nWindows: session_name\\twindow_index\\twindow_id\\twindow_name\\tactive\\tpanes\n";
    out += windows.value().stdout_text;
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out += "\nPanes: session_name\\twindow_index\\twindow_name\\tpane_index\\tpane_id\\tactive\\tcurrent_command\\ttitle\\tcurrent_path\n";
    out += panes.value().stdout_text;
    return Result<std::string>::success(std::move(out));
}

Result<PaneInfo> Tmux::pane_info(const std::string& target) const
{
    const std::string fmt = "#{pane_id}\t#{session_name}\t#{window_index}\t#{pane_index}\t#{pane_current_command}\t#{pane_title}\t#{pane_active}\t#{pane_dead}\t#{pane_current_path}";
    const auto result = run_tmux({"display-message", "-p", "-t", target, fmt});
    if (!result) {
        return Result<PaneInfo>::failure(result.error());
    }
    if (result.value().exit_code != 0) {
        return Result<PaneInfo>::failure(command_failure("tmux display-message", result.value()));
    }

    const auto fields = split_tabs(result.value().stdout_text);
    if (fields.size() < 9 || fields[0].empty()) {
        return Result<PaneInfo>::failure("tmux returned malformed pane info");
    }

    PaneInfo info;
    info.pane_id = fields[0];
    info.session_name = fields[1];
    info.window_index = fields[2];
    info.pane_index = fields[3];
    info.current_command = fields[4];
    info.title = fields[5];
    info.active = fields[6];
    info.dead = fields[7];
    info.current_path = fields[8];
    return Result<PaneInfo>::success(std::move(info));
}

Result<std::string> Tmux::capture_pane(const std::string& target, int lines) const
{
    if (lines < 1) {
        lines = 1;
    }
    const auto result = run_tmux({"capture-pane", "-p", "-J", "-t", target, "-S", "-" + std::to_string(lines)});
    if (!result) {
        return Result<std::string>::failure(result.error());
    }
    if (result.value().exit_code != 0) {
        return Result<std::string>::failure(command_failure("tmux capture-pane", result.value()));
    }
    return Result<std::string>::success(result.value().stdout_text);
}

Result<void> Tmux::send_text(const std::string& target, const std::string& text, const std::string& buffer_name) const
{
    std::string payload = text;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    const auto load = run_tmux({"load-buffer", "-b", buffer_name, "-"}, payload);
    if (!load) {
        return Result<void>::failure(load.error());
    }
    if (load.value().exit_code != 0) {
        return Result<void>::failure(command_failure("tmux load-buffer", load.value()));
    }

    const auto paste = run_tmux({"paste-buffer", "-d", "-b", buffer_name, "-t", target});
    if (!paste) {
        return Result<void>::failure(paste.error());
    }
    if (paste.value().exit_code != 0) {
        return Result<void>::failure(command_failure("tmux paste-buffer", paste.value()));
    }
    return Result<void>::success();
}

Result<void> Tmux::send_ctrl_c(const std::string& target) const
{
    return send_key(target, "C-c");
}

Result<void> Tmux::send_key(const std::string& target, const std::string& key) const
{
    const auto result = run_tmux({"send-keys", "-t", target, key});
    if (!result) {
        return Result<void>::failure(result.error());
    }
    if (result.value().exit_code != 0) {
        return Result<void>::failure(command_failure("tmux send-keys", result.value()));
    }
    return Result<void>::success();
}

} // namespace jjmcp
