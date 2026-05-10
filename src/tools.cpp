#include "tools.hpp"

#include "julia_wrap.hpp"
#include "log.hpp"
#include "process.hpp"
#include "socket_client.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace jjmcp {
namespace {

constexpr int kDefaultTimeoutMs = 10000;
constexpr int kDefaultCaptureLines = 2000;
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

nlohmann::json tool_schema(std::string name, std::string description, nlohmann::json input_schema,
                           nlohmann::json output_schema)
{
    return {
        {"name", std::move(name)},
        {"description", std::move(description)},
        {"inputSchema", std::move(input_schema)},
        {"outputSchema", std::move(output_schema)},
    };
}

nlohmann::json eval_output_schema()
{
    // Describes the structuredContent emitted by eval / revise / activate / test. Optional per the
    // MCP spec; clients that ignore it still receive a usable text content payload.
    return {
        {"type", "object"},
        {"properties",
         {
             {"elapsed_ms", {{"type", "integer"}, {"description", "wall-clock time from send to return"}}},
             {"timed_out", {{"type", "boolean"}}},
             {"found_begin", {{"type", "boolean"}}},
             {"found_end", {{"type", "boolean"}}},
             {"julia_error", {{"type", "boolean"}}},
             {"marker_id", {{"type", "string"}}},
             {"stdout", {{"type", "string"}, {"description", "user code stdout output before OUT_END sentinel"}}},
             {"value_repr", {{"type", "string"}, {"description", "show(MIME\"text/plain\", result) output"}}},
             {"error_message", {{"type", "string"}, {"description", "showerror message without backtrace"}}},
             {"backtrace", {{"type", "string"}, {"description", "Base.show_backtrace output"}}},
             {"transport", {{"type", "string"}, {"description", "tmux or socket"}}},
         }},
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

// Run `julia -e '...'` with the user's code on stdin, asking Julia to parse it
// without executing anything. Returns the parser diagnostic text on failure. Bounded to 2 seconds
// so a Julia start-up hang cannot stall the eval pipeline. The user must have julia on PATH; if
// not, validation is skipped (we cannot know, so we trust the agent and the live REPL).
Result<void> validate_julia_syntax(const std::string& code)
{
    RunSpec spec;
    spec.argv = {"julia", "--color=no", "-e",
                 "try; Meta.parse(read(stdin, String); raise=true); print(\"ok\"); catch e; "
                 "showerror(stderr, e); exit(1); end"};
    spec.stdin_data = code;
    spec.timeout = std::chrono::milliseconds(2000);
    const ProcessRunner runner;
    const auto result = runner.run(spec);
    if (!result) {
        return Result<void>::failure("syntax validator did not run: " + result.error());
    }
    const auto& r = result.value();
    if (r.timed_out) {
        // Treat as a non-fatal skip: the user's Julia is slow to start, fall back to runtime parse.
        return Result<void>::success();
    }
    if (r.exit_code != 0) {
        std::string detail = r.stderr_text.empty() ? r.stdout_text : r.stderr_text;
        if (detail.empty()) detail = "Julia parser rejected the code (exit " + std::to_string(r.exit_code) + ")";
        return Result<void>::failure(detail);
    }
    return Result<void>::success();
}

// Detect known non-julia REPL modes (pkg, help, shell) from the last pane line. The Julia REPL
// paints these prompts in distinct colors to humans, but capture-pane (without -e) strips ANSI
// codes, so we substring-match against the trimmed text. The pkg prompt format includes the
// active environment in parentheses, e.g. "(@v1.11) pkg> ", so we only match the prompt suffix.
bool is_known_non_julia_prompt(const std::string& last_line)
{
    const auto trimmed = trim_ascii(last_line);
    if (trimmed.find("pkg> ") != std::string::npos) return true;
    if (trimmed.find("help?> ") != std::string::npos) return true;
    if (trimmed.find("shell> ") != std::string::npos) return true;
    return false;
}

// RAII advisory lock at <config_dir>/lock. Protects the bound pane against concurrent eval_code
// calls from multiple jjmcp processes (different agents) competing for the same Julia REPL.
// Stale-lock detection: if the recorded pid is no longer alive, we treat the lock as orphaned and
// reclaim it. This is best-effort; the kernel cannot tell us whether a freshly recycled pid is the
// same process, so a vanishingly small race window remains.
class AdvisoryLock {
public:
    explicit AdvisoryLock(std::filesystem::path path) : path_(std::move(path)) {}
    AdvisoryLock(const AdvisoryLock&) = delete;
    AdvisoryLock& operator=(const AdvisoryLock&) = delete;
    ~AdvisoryLock() { release(); }

    Result<void> acquire(const std::string& marker_id)
    {
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int fd = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd >= 0) {
                fd_ = fd;
                const std::string content = std::to_string(static_cast<long long>(::getpid())) + " "
                                            + marker_id + "\n";
                const auto written = ::write(fd, content.data(), content.size());
                (void)written;  // best-effort; the lock is in place even if write fails
                return Result<void>::success();
            }
            if (errno != EEXIST) {
                return Result<void>::failure(std::string("could not open lock file: ")
                                             + std::strerror(errno));
            }
            // Lock exists; check if it is stale (process gone).
            std::ifstream f(path_);
            long long pid = 0;
            f >> pid;
            if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH) {
                ::unlink(path_.c_str());
                continue;
            }
            return Result<void>::failure("an evaluation is already in progress (lock at "
                                         + path_.string() + ")");
        }
        return Result<void>::failure("could not acquire advisory lock after retry");
    }

    void release()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            ::unlink(path_.c_str());
            fd_ = -1;
        }
    }

private:
    std::filesystem::path path_;
    int fd_ = -1;
};

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
        "Evaluate Julia code in the bound REPL. transport=auto prefers a JJMCPHelper.jl Unix socket "
        "(faster, structured native) when available and falls back to the tmux marker path otherwise.",
        object_schema(
            {
                {"code", {{"type", "string"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_timeout_ms}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
                {"force", {{"type", "boolean"}, {"description", "skip pane-mode pre-check (default false)"}}},
                {"validate_syntax", {{"type", "boolean"}, {"description", "parse code via a 2s julia Meta.parse before paste (default true)"}}},
                {"transport", {{"type", "string"}, {"enum", nlohmann::json::array({"auto", "tmux", "socket"})}, {"description", "auto (default), tmux (force marker path), or socket (require JJMCPHelper.jl)"}}},
            },
            nlohmann::json::array({"code"})),
        eval_output_schema()));

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
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_timeout_ms}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            }),
        eval_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_activate",
        "Run using Pkg; Pkg.activate(path) in the bound Julia pane.",
        object_schema(
            {
                {"path", {{"type", "string"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_timeout_ms}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            },
            nlohmann::json::array({"path"})),
        eval_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_test",
        "Run focused Julia tests in the existing REPL context. Precedence: test_expr > file > "
        "test_item_pattern (TestItemRunner) > Pkg.test().",
        object_schema(
            {
                {"test_expr", {{"type", "string"}, {"description", "raw Julia test expression to evaluate"}}},
                {"file", {{"type", "string"}, {"description", "path to a test file to include()"}}},
                {"test_item_pattern", {{"type", "string"}, {"description", "substring match against TestItemRunner @testitem names"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_timeout_ms}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            }),
        eval_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_pkg_status",
        "Run `using Pkg; Pkg.status()` in the bound Julia pane and return the status text in the structured stdout field.",
        object_schema(
            {
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_timeout_ms}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            }),
        eval_output_schema()));

    return tools;
}

ToolResult ToolDispatcher::call(const std::string& name, const nlohmann::json& arguments,
                                ProgressEmitter* progress)
{
    current_progress_ = progress;
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
    if (name == "jjmcp_pkg_status") {
        return pkg_status(arguments);
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
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, state_.max_timeout_ms);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    const bool force = arguments.value("force", false);
    const bool validate_syntax = arguments.value("validate_syntax", true);
    const std::string transport = optional_string(arguments, "transport", "auto");
    if (transport != "auto" && transport != "tmux" && transport != "socket") {
        return ToolResult::error("transport must be one of: auto, tmux, socket");
    }
    if (validate_syntax) {
        if (const auto ok = validate_julia_syntax(code); !ok) {
            return ToolResult::error("syntax error before eval (set validate_syntax=false to skip):\n"
                                     + ok.error());
        }
    }
    return eval_code(code, timeout_ms, capture_lines, force, nullptr, transport);
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
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, state_.max_timeout_ms);
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
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, state_.max_timeout_ms);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code(make_activate_code(path), timeout_ms, capture_lines);
}

ToolResult ToolDispatcher::test(const nlohmann::json& arguments)
{
    const std::string test_expr = optional_string(arguments, "test_expr");
    const std::string file = optional_string(arguments, "file");
    const std::string test_item_pattern = optional_string(arguments, "test_item_pattern");
    const int timeout_ms = optional_int(arguments, "timeout_ms", 120000, 1, state_.max_timeout_ms);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code(make_test_code(test_expr, file, test_item_pattern), timeout_ms, capture_lines);
}

ToolResult ToolDispatcher::pkg_status(const nlohmann::json& arguments)
{
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultTimeoutMs, 1, state_.max_timeout_ms);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    return eval_code("using Pkg\nPkg.status()", timeout_ms, capture_lines);
}

namespace {

// Build a ToolResult that mirrors the structuredContent shape used by the tmux marker path so the
// agent sees a uniform result regardless of which transport ran the eval.
ToolResult socket_response_to_tool_result(const SocketEvalResponse& resp, const std::string& transport_name)
{
    nlohmann::json structured = {
        {"elapsed_ms", resp.elapsed_ms},
        {"timed_out", false},
        {"found_begin", true},
        {"found_end", true},
        {"julia_error", !resp.ok},
        {"marker_id", ""},
        {"stdout", resp.stdout_text},
        {"value_repr", resp.value_show},
        {"error_message", resp.error_message},
        {"backtrace", resp.backtrace},
        {"transport", transport_name},
    };
    std::string text;
    if (resp.ok) {
        text = resp.stdout_text;
        if (!resp.value_show.empty()) {
            if (!text.empty() && text.back() != '\n') text.push_back('\n');
            text += resp.value_show;
        }
        return ToolResult::success(std::move(text)).with_structured(std::move(structured));
    }
    text = resp.error_message;
    if (!resp.backtrace.empty()) {
        if (!text.empty() && text.back() != '\n') text.push_back('\n');
        text += resp.backtrace;
    }
    return ToolResult::error(std::move(text)).with_structured(std::move(structured));
}

} // namespace

ToolResult ToolDispatcher::eval_code(const std::string& code, int timeout_ms, int capture_lines,
                                     bool force, ProgressEmitter* progress_override,
                                     const std::string& transport)
{
    ProgressEmitter* progress = progress_override != nullptr ? progress_override : current_progress_;
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

    // Transport routing: when transport is "socket" or "auto" and a JJMCPHelper.jl socket exists
    // for the bound pane, prefer that path for structured returns and millisecond-scale latency.
    // The socket path runs eval at top level via Base.eval(Main, ...) and echoes stdout/stderr to
    // the live REPL, so the human-visible scrollback is unchanged. With transport=tmux we always
    // use the marker path. With transport=auto, a socket failure falls back to the marker path
    // with a warn log; with transport=socket, a failure is returned as-is.
    if (transport != "tmux" && state_.binding) {
        const std::string& pane_id = state_.binding->pane_id.empty() ? state_.binding->target : state_.binding->pane_id;
        const std::string sock_path = SocketClient::default_socket_path(pane_id);
        if (SocketClient::socket_exists(sock_path)) {
            const SocketClient client;
            const auto resp = client.eval(sock_path, code, std::chrono::milliseconds(timeout_ms));
            if (resp) {
                return socket_response_to_tool_result(resp.value(), "socket");
            }
            if (transport == "socket") {
                return ToolResult::error("socket transport failed: " + resp.error());
            }
            log::warn("socket_eval_fallback_to_tmux",
                      {{"path", sock_path}, {"error", resp.error()}});
        } else if (transport == "socket") {
            return ToolResult::error("socket transport requested but no JJMCPHelper socket at "
                                     + sock_path
                                     + " (start it with `using JJMCPHelper; JJMCPHelper.start()`)");
        }
    }

    // Pane-mode safety: refuse to paste into a non-julia REPL mode (pkg/help/shell). Try a single
    // backspace to exit common alternate modes, then re-check. force=true bypasses this entirely.
    if (!force) {
        const auto last = tmux_.capture_pane(bound_target(), 1);
        if (last && is_known_non_julia_prompt(last.value())) {
            (void)tmux_.send_key(bound_target(), "BSpace");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto recheck = tmux_.capture_pane(bound_target(), 1);
            if (!recheck || is_known_non_julia_prompt(recheck.value())) {
                return ToolResult::error(
                    std::string("bound pane appears to be in a non-julia REPL mode "
                                "(pkg>/help?>/shell>). Last line: \"")
                    + (recheck ? recheck.value() : last.value())
                    + "\". Press backspace in the pane to return to julia>, or pass force=true.");
            }
        }
    }

    // Acquire advisory lock so a second jjmcp process targeting the same pane cannot interleave
    // its paste-buffer with ours.
    AdvisoryLock lock(state_.config_path.parent_path() / "lock");
    if (const auto acquired = lock.acquire(std::to_string(state_.marker_sequence + 1)); !acquired) {
        return ToolResult::error(acquired.error());
    }

    const Marker marker = make_marker(next_marker_id(state_));
    const std::string wrapped = wrap_julia_code(code, marker);
    const std::string buffer_name = "jjmcp_" + marker.id;
    const auto start = std::chrono::steady_clock::now();
    const auto sent = tmux_.send_text(bound_target(), wrapped, buffer_name);
    if (!sent) {
        return ToolResult::error(sent.error());
    }

    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    const int poll_capture_lines = std::clamp(capture_lines + 500, 1000, kMaxCaptureLines);

    // Adaptive polling: start at 50 ms, double when no growth, reset on growth, cap at 1000 ms.
    // Replaces a fixed 75 ms loop that issued ~4000 capture-pane subprocesses per 5-min Pkg.test.
    //
    // Accumulator: each tmux capture-pane returns the last poll_capture_lines of the visible pane.
    // We splice consecutive captures by their longest suffix-prefix overlap so output that has
    // scrolled off the visible pane buffer (and thus is no longer in the next capture) is still
    // preserved in the working set. Without this, a long compile log can push the BEGIN marker
    // out of the capture window, leaving extract_between_markers().found_begin = false for an
    // eval that actually succeeded.
    std::chrono::milliseconds backoff{50};
    constexpr std::chrono::milliseconds kBackoffMin{50};
    constexpr std::chrono::milliseconds kBackoffMax{1000};
    std::string accumulator;
    std::string last_capture;
    ExtractedOutput last_extract;
    bool timed_out = false;
    bool begin_trimmed = false;

    constexpr std::size_t kProgressMinDelta = 4096;
    constexpr std::size_t kProgressMaxTail = 4096;
    std::size_t last_progress_size = 0;
    int progress_count = 0;

    while (true) {
        const auto captured = tmux_.capture_pane(bound_target(), poll_capture_lines);
        if (!captured) {
            return ToolResult::error(captured.error());
        }
        const std::string& cap = captured.value();

        bool grew = false;
        if (accumulator.empty()) {
            if (!cap.empty()) {
                accumulator = cap;
                grew = true;
            }
        } else if (cap != last_capture) {
            const std::size_t overlap = compute_capture_overlap(last_capture, cap);
            if (overlap < cap.size()) {
                accumulator.append(cap, overlap, cap.size() - overlap);
                grew = true;
            }
        }
        last_capture = cap;

        // Prefer extracting from the live capture: it is the most authoritative source for the
        // common small-pane / not-yet-scrolled case and is immune to any imprecision in the
        // accumulator splice. If BEGIN has already scrolled off the live capture, fall back to
        // the accumulator, which retains everything since the first capture that saw BEGIN.
        last_extract = extract_between_markers(cap, marker);
        if (!last_extract.found_begin) {
            last_extract = extract_between_markers(accumulator, marker);
        }

        // Once BEGIN is located anywhere, discard all pre-BEGIN pane history from the accumulator.
        // Future extraction passes only need to scan from BEGIN onward, keeping the working set
        // bounded by actual output size rather than total pane history.
        if (!begin_trimmed && last_extract.found_begin) {
            const auto begin_pos = accumulator.find(marker.begin);
            if (begin_pos != std::string::npos) {
                const auto line_start = accumulator.rfind('\n', begin_pos);
                const std::size_t trim_to = (line_start == std::string::npos) ? 0 : line_start + 1;
                if (trim_to > 0) {
                    accumulator.erase(0, trim_to);
                }
            }
            begin_trimmed = true;
        }

        // Emit a progress notification when the in-progress output between markers grows by at
        // least kProgressMinDelta bytes. Cap the tail per notification to avoid megabyte frames
        // when a large value is printed all at once.
        if (progress != nullptr && progress->active() && last_extract.found_begin) {
            const std::size_t cur = last_extract.text.size();
            if (cur >= last_progress_size + kProgressMinDelta) {
                std::string tail = last_extract.text.substr(last_progress_size);
                if (tail.size() > kProgressMaxTail) {
                    tail = tail.substr(tail.size() - kProgressMaxTail);
                }
                ++progress_count;
                progress->emit(static_cast<double>(progress_count), tail);
                last_progress_size = cur;
            }
        }

        if (last_extract.found_end) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;
        }

        backoff = grew ? kBackoffMin : std::min(backoff * 2, kBackoffMax);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(std::min(backoff, remaining));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();


    const auto cap_lines = static_cast<std::size_t>(capture_lines);
    nlohmann::json structured = {
        {"elapsed_ms", elapsed},
        {"timed_out", timed_out},
        {"found_begin", last_extract.found_begin},
        {"found_end", last_extract.found_end},
        {"julia_error", last_extract.julia_error},
        {"marker_id", marker.id},
        {"stdout",        truncate_lines(last_extract.stdout_text,    cap_lines)},
        {"value_repr",    truncate_lines(last_extract.value_repr,     cap_lines)},
        {"error_message", truncate_lines(last_extract.error_message,  cap_lines)},
        {"backtrace",     truncate_lines(last_extract.backtrace,      200)},
        {"transport", "tmux"},
    };

    if (!timed_out) {
        auto text = truncate_lines(last_extract.text, static_cast<std::size_t>(capture_lines));
        ToolResult result = last_extract.julia_error ? ToolResult::error(std::move(text))
                                                     : ToolResult::success(std::move(text));
        return result.with_structured(std::move(structured));
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
    return ToolResult::error(out.str()).with_structured(std::move(structured));
}

namespace {

constexpr const char* kResourceScheme = "jjmcp://project/";

struct ResourceSpec {
    std::string filename;
    std::string description;
};

const std::vector<ResourceSpec>& resource_catalog()
{
    static const std::vector<ResourceSpec> catalog = {
        {"Project.toml", "Top-level package metadata for the bound project"},
        {"Manifest.toml", "Resolved dependency manifest for the bound project"},
        {"JuliaProject.toml", "Alternate top-level metadata when Project.toml is unused"},
        {"JuliaManifest.toml", "Alternate resolved manifest"},
    };
    return catalog;
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

nlohmann::json ToolDispatcher::list_resources_json() const
{
    nlohmann::json resources = nlohmann::json::array();
    if (!state_.binding || state_.binding->project_root.empty()) {
        return resources;
    }
    const std::filesystem::path root = state_.binding->project_root;
    for (const auto& spec : resource_catalog()) {
        std::error_code ec;
        if (std::filesystem::exists(root / spec.filename, ec)) {
            resources.push_back({
                {"uri", std::string(kResourceScheme) + spec.filename},
                {"name", spec.filename},
                {"description", spec.description},
                {"mimeType", "application/toml"},
            });
        }
    }
    return resources;
}

Result<nlohmann::json> ToolDispatcher::read_resource(const std::string& uri) const
{
    if (uri.rfind(kResourceScheme, 0) != 0) {
        return Result<nlohmann::json>::failure("unsupported URI scheme: " + uri);
    }
    if (!state_.binding || state_.binding->project_root.empty()) {
        return Result<nlohmann::json>::failure("no project root bound; call jjmcp_bind with project_root");
    }
    const std::string filename = uri.substr(std::strlen(kResourceScheme));
    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
        return Result<nlohmann::json>::failure("resource path must be a single filename");
    }
    bool known = false;
    for (const auto& spec : resource_catalog()) {
        if (spec.filename == filename) {
            known = true;
            break;
        }
    }
    if (!known) {
        return Result<nlohmann::json>::failure("unknown resource: " + filename);
    }
    const std::filesystem::path path = std::filesystem::path(state_.binding->project_root) / filename;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Result<nlohmann::json>::failure("resource not found at " + path.string());
    }
    nlohmann::json contents = nlohmann::json::array();
    contents.push_back({
        {"uri", uri},
        {"mimeType", "application/toml"},
        {"text", read_text_file(path)},
    });
    return Result<nlohmann::json>::success({{"contents", std::move(contents)}});
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
