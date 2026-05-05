#include "tools.hpp"

#include "julia_wrap.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace jjmcp {
namespace {

constexpr int kDefaultTimeoutMs = 10000;
constexpr int kDefaultCaptureLines = 2000;
constexpr int kMaxTimeoutMs = 600000;
constexpr int kMaxCaptureLines = 50000;

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required = nlohmann::json::array())
{
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

nlohmann::json tool_schema(std::string name, std::string description, nlohmann::json input_schema)
{
    return {
        {"name", std::move(name)},
        {"description", std::move(description)},
        {"inputSchema", std::move(input_schema)},
    };
}

std::string require_string(const nlohmann::json& args, const char* key, std::string& error)
{
    if (!args.contains(key) || !args[key].is_string()) {
        error = std::string("argument '") + key + "' must be a string";
        return {};
    }
    return args[key].get<std::string>();
}

std::string optional_string(const nlohmann::json& args, const char* key, std::string default_value = {})
{
    if (!args.contains(key) || args[key].is_null()) {
        return default_value;
    }
    if (!args[key].is_string()) {
        return default_value;
    }
    return args[key].get<std::string>();
}

int optional_int(const nlohmann::json& args, const char* key, int default_value, int min_value, int max_value)
{
    if (!args.contains(key) || !args[key].is_number_integer()) {
        return default_value;
    }
    return std::clamp(args[key].get<int>(), min_value, max_value);
}

std::filesystem::path config_base_from_project(const std::filesystem::path& cwd, const std::string& project_root)
{
    if (project_root.empty()) {
        return cwd;
    }
    std::filesystem::path path(project_root);
    if (path.is_relative()) {
        path = cwd / path;
    }
    return path.lexically_normal();
}

std::string format_pane_info(const PaneInfo& info)
{
    std::ostringstream out;
    out << "pane_id: " << info.pane_id << '\n';
    out << "session: " << info.session_name << '\n';
    out << "window_index: " << info.window_index << '\n';
    out << "pane_index: " << info.pane_index << '\n';
    out << "current_command: " << info.current_command << '\n';
    out << "title: " << info.title << '\n';
    out << "active: " << info.active << '\n';
    out << "dead: " << info.dead << '\n';
    out << "current_path: " << info.current_path << '\n';
    return out.str();
}

} // namespace

ToolDispatcher::ToolDispatcher(ServerState& state, const Tmux& tmux, std::filesystem::path cwd)
    : state_(state), tmux_(tmux), cwd_(std::move(cwd))
{
}

nlohmann::json ToolDispatcher::list_tools_json() const
{
    nlohmann::json tools = nlohmann::json::array();

    tools.push_back(tool_schema(
        "jjmcp_list_tmux",
        "List tmux sessions, windows, panes, pane ids, active processes, titles, and current paths.",
        object_schema(nlohmann::json::object())));

    tools.push_back(tool_schema(
        "jjmcp_bind",
        "Bind JohnJuliaMCP to an existing tmux pane that is expected to run Julia.",
        object_schema(
            {
                {"target", {{"type", "string"}, {"description", "tmux target pane, for example session:window.pane or %pane_id"}}},
                {"project_root", {{"type", "string"}, {"description", "Optional project root for .jjmcp/config.json persistence"}}},
            },
            nlohmann::json::array({"target"}))));

    tools.push_back(tool_schema(
        "jjmcp_status",
        "Report current binding, pane info, last marker, last command timestamp, and liveness.",
        object_schema(nlohmann::json::object())));

    tools.push_back(tool_schema(
        "jjmcp_eval",
        "Evaluate Julia code in the bound tmux pane and capture output between unique markers.",
        object_schema(
            {
                {"code", {{"type", "string"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxTimeoutMs}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            },
            nlohmann::json::array({"code"}))));

    tools.push_back(tool_schema(
        "jjmcp_capture",
        "Capture recent output from the bound tmux pane without sending input.",
        object_schema({{"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}}})));

    tools.push_back(tool_schema(
        "jjmcp_interrupt",
        "Send Ctrl-C to the bound tmux pane.",
        object_schema(nlohmann::json::object())));

    tools.push_back(tool_schema(
        "jjmcp_revise",
        "Run Revise.revise() in the bound Julia pane.",
        object_schema(
            {
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxTimeoutMs}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            })));

    tools.push_back(tool_schema(
        "jjmcp_activate",
        "Run using Pkg; Pkg.activate(path) in the bound Julia pane.",
        object_schema(
            {
                {"path", {{"type", "string"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxTimeoutMs}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            },
            nlohmann::json::array({"path"}))));

    tools.push_back(tool_schema(
        "jjmcp_test",
        "Run focused Julia tests in the existing REPL context. Provide test_expr, file, or neither for Pkg.test().",
        object_schema(
            {
                {"test_expr", {{"type", "string"}}},
                {"file", {{"type", "string"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxTimeoutMs}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            })));

    return tools;
}

ToolResult ToolDispatcher::call(const std::string& name, const nlohmann::json& arguments)
{
    if (name == "jjmcp_list_tmux") {
        return list_tmux();
    }
    if (name == "jjmcp_bind") {
        return bind(arguments);
    }
    if (name == "jjmcp_status") {
        return status();
    }
    if (name == "jjmcp_eval") {
        return eval(arguments);
    }
    if (name == "jjmcp_capture") {
        return capture(arguments);
    }
    if (name == "jjmcp_interrupt") {
        return interrupt();
    }
    if (name == "jjmcp_revise") {
        return revise(arguments);
    }
    if (name == "jjmcp_activate") {
        return activate(arguments);
    }
    if (name == "jjmcp_test") {
        return test(arguments);
    }
    return ToolResult::error("unknown tool: " + name);
}

ToolResult ToolDispatcher::list_tmux() const
{
    const auto listed = tmux_.list_all();
    if (!listed) {
        return ToolResult::error(listed.error());
    }
    return ToolResult::success(listed.value());
}

ToolResult ToolDispatcher::bind(const nlohmann::json& arguments)
{
    std::string error;
    const std::string target = require_string(arguments, "target", error);
    if (!error.empty()) {
        return ToolResult::error(error);
    }
    const std::string project_root = optional_string(arguments, "project_root");

    const auto info = tmux_.pane_info(target);
    if (!info) {
        return ToolResult::error(info.error());
    }
    if (!info.value().alive()) {
        return ToolResult::error("tmux pane exists but appears dead: " + target);
    }

    Binding binding;
    binding.target = target;
    binding.pane_id = info.value().pane_id;
    binding.project_root = project_root;
    binding.bound_at = timestamp_now();

    const auto config_base = config_base_from_project(cwd_, project_root);
    set_binding(state_, std::move(binding), config_base);
    const auto saved = save_config(state_);

    std::ostringstream out;
    out << "Bound to tmux pane " << state_.binding->pane_id << '\n';
    out << format_pane_info(info.value());
    out << "config_path: " << state_.config_path.string() << '\n';
    if (!saved) {
        out << "config_warning: " << saved.error() << '\n';
    }
    return ToolResult::success(out.str());
}

ToolResult ToolDispatcher::status() const
{
    std::ostringstream out;
    out << "config_path: " << state_.config_path.string() << '\n';
    out << "last_marker: " << state_.last_marker << '\n';
    out << "last_command_timestamp: " << state_.last_command_timestamp << '\n';

    if (!state_.binding) {
        out << "binding: none\n";
        return ToolResult::success(out.str());
    }

    out << "binding_target: " << state_.binding->target << '\n';
    out << "binding_pane_id: " << state_.binding->pane_id << '\n';
    out << "project_root: " << state_.binding->project_root << '\n';
    out << "bound_at: " << state_.binding->bound_at << '\n';

    const auto info = tmux_.pane_info(bound_target());
    if (!info) {
        out << "alive: false\n";
        out << "pane_error: " << info.error() << '\n';
        return ToolResult::success(out.str());
    }

    out << "alive: " << (info.value().alive() ? "true" : "false") << '\n';
    out << format_pane_info(info.value());
    return ToolResult::success(out.str());
}

ToolResult ToolDispatcher::eval(const nlohmann::json& arguments)
{
    std::string error;
    const std::string code = require_string(arguments, "code", error);
    if (!error.empty()) {
        return ToolResult::error(error);
    }
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code(code, timeout_ms, capture_lines);
}

ToolResult ToolDispatcher::capture(const nlohmann::json& arguments) const
{
    if (!state_.binding) {
        return ToolResult::error("no tmux pane is bound; call jjmcp_bind first");
    }
    const int lines = optional_int(arguments, "lines", 200, 1, kMaxCaptureLines);
    const auto captured = tmux_.capture_pane(bound_target(), lines);
    if (!captured) {
        return ToolResult::error(captured.error());
    }
    return ToolResult::success(truncate_lines_tail(captured.value(), static_cast<std::size_t>(lines)));
}

ToolResult ToolDispatcher::interrupt() const
{
    if (!state_.binding) {
        return ToolResult::error("no tmux pane is bound; call jjmcp_bind first");
    }
    const auto sent = tmux_.send_ctrl_c(bound_target());
    if (!sent) {
        return ToolResult::error(sent.error());
    }
    return ToolResult::success("Sent Ctrl-C to " + bound_target());
}

ToolResult ToolDispatcher::revise(const nlohmann::json& arguments)
{
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code(make_revise_code(), timeout_ms, capture_lines);
}

ToolResult ToolDispatcher::activate(const nlohmann::json& arguments)
{
    std::string error;
    const std::string path = require_string(arguments, "path", error);
    if (!error.empty()) {
        return ToolResult::error(error);
    }
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code(make_activate_code(path), timeout_ms, capture_lines);
}

ToolResult ToolDispatcher::test(const nlohmann::json& arguments)
{
    const std::string test_expr = optional_string(arguments, "test_expr");
    const std::string file = optional_string(arguments, "file");
    const int timeout_ms = optional_int(arguments, "timeout_ms", 120000, 1, kMaxTimeoutMs);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code(make_test_code(test_expr, file), timeout_ms, capture_lines);
}

ToolResult ToolDispatcher::eval_code(const std::string& code, int timeout_ms, int capture_lines)
{
    if (!state_.binding) {
        return ToolResult::error("no tmux pane is bound; call jjmcp_bind first");
    }

    const auto info = tmux_.pane_info(bound_target());
    if (!info) {
        return ToolResult::error(info.error());
    }
    if (!info.value().alive()) {
        return ToolResult::error("bound tmux pane appears dead: " + bound_target());
    }

    const Marker marker = make_marker(next_marker_id(state_));
    const std::string wrapped = wrap_julia_code(code, marker);
    const std::string buffer_name = "jjmcp_" + marker.id;
    const auto sent = tmux_.send_text(bound_target(), wrapped, buffer_name);
    if (!sent) {
        return ToolResult::error(sent.error());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const int poll_capture_lines = std::clamp(capture_lines + 500, 1000, kMaxCaptureLines);
    ExtractedOutput last_extract;
    std::string last_capture;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto captured = tmux_.capture_pane(bound_target(), poll_capture_lines);
        if (!captured) {
            return ToolResult::error(captured.error());
        }
        last_capture = captured.value();
        last_extract = extract_between_markers(last_capture, marker);
        if (last_extract.found_end) {
            auto text = truncate_lines(last_extract.text, static_cast<std::size_t>(capture_lines));
            return last_extract.julia_error ? ToolResult::error(text) : ToolResult::success(text);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }

    std::ostringstream out;
    out << "Timed out after " << timeout_ms << " ms waiting for " << marker.end;
    if (last_extract.found_begin && !last_extract.text.empty()) {
        out << "\nPartial output between markers:\n";
        out << truncate_lines(last_extract.text, static_cast<std::size_t>(capture_lines));
    } else if (!last_capture.empty()) {
        out << "\nRecent pane output:\n";
        out << truncate_lines_tail(last_capture, static_cast<std::size_t>(std::min(capture_lines, 200)));
    }
    return ToolResult::error(out.str());
}

std::string ToolDispatcher::bound_target() const
{
    if (!state_.binding) {
        return {};
    }
    if (!state_.binding->pane_id.empty()) {
        return state_.binding->pane_id;
    }
    return state_.binding->target;
}

} // namespace jjmcp
