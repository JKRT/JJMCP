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
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <cctype>
#include <optional>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace jjmcp {
namespace {

constexpr int kDefaultTimeoutMs = 10000;
constexpr int kDefaultCaptureLines = 2000;
constexpr int kMaxCaptureLines = 50000;
constexpr std::size_t kMaxSocketFieldBytes = 240 * 1024;
// A submitted job is polled in the foreground only long enough to catch a REPL that rejects the
// wrapper outright; after that the background poller owns it.
constexpr int kJobBootstrapWindowMs = 1500;
constexpr int kDefaultJobTimeoutMs = 3600000;
constexpr int kDefaultWaitMs = 60000;
constexpr int kDefaultStatusLines = 200;

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

// Superset of eval_output_schema: every field the eval path already emitted, plus the job identity
// and liveness fields that make a result recoverable after the waiting window closes.
nlohmann::json job_output_schema()
{
    nlohmann::json schema = eval_output_schema();
    auto& properties = schema["properties"];
    properties["job_id"] = {{"type", "string"}, {"description", "equal to marker_id"}};
    properties["state"] = {
        {"type", "string"},
        {"enum", nlohmann::json::array({"running", "completed", "failed", "timed_out"})},
        {"description", "running means the job is still being polled; completed with julia_error=true means Julia threw"},
    };
    properties["target"] = {{"type", "string"}};
    properties["submitted_at"] = {{"type", "string"}};
    properties["detached"] = {{"type", "boolean"}, {"description", "polled by a background thread"}};
    properties["timeout_ms"] = {{"type", "integer"}};
    properties["output_bytes"] = {{"type", "integer"}};
    properties["live_tail"] = {{"type", "string"}, {"description", "tail of the output so far, running jobs only"}};
    properties["failure"] = {{"type", "string"}, {"description", "why jjmcp lost the job, not a Julia error"}};
    properties["last_output_unix_ms"] = {{"type", "integer"}};
    properties["last_output_age_ms"] = {{"type", "integer"}};
    properties["repl_pid"] = {{"type", "integer"}, {"description", "foreground process group of the pane"}};
    properties["cpu_seconds"] = {{"type", "number"}};
    properties["rss_bytes"] = {{"type", "integer"}};
    properties["proc_state"] = {{"type", "string"}};
    properties["from_result_store"] = {{"type", "boolean"}};
    properties["recovered_from_scrollback"] = {
        {"type", "boolean"},
        {"description", "true when the result was rebuilt from pane history, which may be incomplete"},
    };
    return schema;
}

nlohmann::json test_summary_output_schema()
{
    return {
        {"type", "object"},
        {"properties",
         {
             {"found_summary", {"type", "boolean"}},
             {"test_pass", {"type", "integer"}},
             {"test_fail", {"type", "integer"}},
             {"test_broken", {"type", "integer"}},
             {"test_error", {"type", "integer"}},
             {"test_total", {"type", "integer"}},
             {"test_time", {"type", "string"}},
             {
                 "status",
                 {{"type", "string"}, {"enum", nlohmann::json::array({"pass", "fail", "unknown"})}},
             },
             {"failures", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"raw_output", {"type", "string"}},
             {"capture_lines", {"type", "integer"}},
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

std::string to_lower(std::string value)
{
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::vector<std::string> summary_tokens(const std::string& text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (char c : text) {
        normalized.push_back(c == '|' ? ' ' : c);
    }
    std::istringstream iss(normalized);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool is_int_token(const std::string& token)
{
    static const std::regex kInt{R"(^\d+$)"};
    return std::regex_match(token, kInt);
}

bool is_float_token(const std::string& token)
{
    static const std::regex kFloat{R"(^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$)"};
    return std::regex_match(token, kFloat);
}

std::vector<std::string> normalize_summary_columns(const std::string& summary_line)
{
    std::string tail = summary_line;
    const auto marker_pos = tail.find("Test Summary:");
    if (marker_pos != std::string::npos) {
        tail = tail.substr(marker_pos + std::string("Test Summary:").size());
    }
    if (tail.empty()) {
        return {};
    }
    const auto pipe_pos = tail.find('|');
    if (pipe_pos != std::string::npos) {
        tail = tail.substr(pipe_pos + 1);
    }

    std::vector<std::string> columns;
    for (auto& token : summary_tokens(tail)) {
        const auto name = to_lower(token);
        if (name == "pass" || name == "passes") {
            columns.push_back("pass");
        } else if (name == "fail" || name == "fails") {
            columns.push_back("fail");
        } else if (name == "broken") {
            columns.push_back("broken");
        } else if (name == "error" || name == "errors") {
            columns.push_back("error");
        } else if (name == "total" || name == "tot") {
            columns.push_back("total");
        } else if (name == "time") {
            columns.push_back("time");
        }
    }

    if (columns.empty()) {
        return {};
    }
    return columns;
}

bool parse_summary_counts_row(const std::vector<std::string>& columns, const std::string& line,
                             TestSummaryResult& out)
{
    TestSummaryResult tmp;
    bool parsed_pass = false;
    bool parsed_fail = false;
    bool parsed_broken = false;
    bool parsed_error = false;
    bool parsed_total = false;
    bool parsed_any = false;

    const auto tokens = summary_tokens(line);
    if (tokens.empty()) {
        return false;
    }

    std::size_t cursor = 0;
    auto next_integer = [&]() -> int {
        for (; cursor < tokens.size(); ++cursor) {
            if (is_int_token(tokens[cursor])) {
                // Pane text is arbitrary, so a digit run can exceed int; clamp instead of throwing.
                const long long parsed = std::strtoll(tokens[cursor++].c_str(), nullptr, 10);
                return static_cast<int>(std::clamp<long long>(parsed, 0, 1000000000));
            }
        }
        return -1;
    };

    for (const auto& col : columns) {
        if (col == "pass") {
            const auto v = next_integer();
            if (v < 0) return false;
            tmp.test_pass = v;
            parsed_pass = true;
            parsed_any = true;
        } else if (col == "fail") {
            const auto v = next_integer();
            if (v < 0) return false;
            tmp.test_fail = v;
            parsed_fail = true;
            parsed_any = true;
        } else if (col == "broken") {
            const auto v = next_integer();
            if (v < 0) return false;
            tmp.test_broken = v;
            parsed_broken = true;
            parsed_any = true;
        } else if (col == "error") {
            const auto v = next_integer();
            if (v < 0) return false;
            tmp.test_error = v;
            parsed_error = true;
            parsed_any = true;
        } else if (col == "total") {
            const auto v = next_integer();
            if (v < 0) return false;
            tmp.test_total = v;
            parsed_total = true;
            parsed_any = true;
        } else if (col == "time") {
            if (cursor >= tokens.size()) {
                return false;
            }
            tmp.test_time = tokens[cursor++];
            if (cursor < tokens.size() && !is_int_token(tokens[cursor]) && !is_float_token(tokens[cursor])) {
                tmp.test_time += " " + tokens[cursor++];
            }
        }
    }

    if (!parsed_any) {
        return false;
    }
    if (!parsed_total && (parsed_pass || parsed_fail || parsed_broken || parsed_error)) {
        tmp.test_total = tmp.test_pass + tmp.test_fail + tmp.test_broken + tmp.test_error;
        parsed_total = true;
    }

    if (!parsed_total || !parsed_any) {
        return false;
    }

    if (!parsed_fail && !parsed_broken && !parsed_error) {
        tmp.status = "pass";
    } else {
        tmp.status = "fail";
    }

    tmp.found_summary = true;

    out = std::move(tmp);
    return true;
}

std::vector<std::string> extract_failure_snippets(const std::vector<std::string>& lines)
{
    static const std::string kMarker = "Test Failed at";
    std::vector<std::string> failures;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(kMarker) == std::string::npos) {
            continue;
        }
        const auto start = (i == 0) ? 0 : i - 1;
        const auto end = std::min(i + 1, lines.size() > 0 ? lines.size() - 1 : 0);
        std::string snippet;
        for (std::size_t j = start; j <= end; ++j) {
            const auto trimmed = trim_ascii(lines[j]);
            if (trimmed.empty()) {
                continue;
            }
            if (!snippet.empty()) {
                snippet.push_back('\n');
            }
            snippet += trimmed;
        }
        if (!snippet.empty()) {
            if (failures.empty() || failures.back() != snippet) {
                failures.push_back(std::move(snippet));
            }
        }
    }
    return failures;
}

TestSummaryResult parse_test_summary_impl(const std::string& capture)
{
    TestSummaryResult result;
    const auto lines = split_lines(capture);
    result.failures = extract_failure_snippets(lines);

    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
        if (lines[i].find("Test Summary:") == std::string::npos) {
            continue;
        }
        std::vector<std::vector<std::string>> candidates;
        {
            const auto cols = normalize_summary_columns(lines[i]);
            if (!cols.empty()) {
                candidates.push_back(cols);
            }
        }
        if (i + 1 < static_cast<int>(lines.size())) {
            const auto header_cols = normalize_summary_columns(lines[i + 1]);
            if (!header_cols.empty()) {
                candidates.push_back(header_cols);
            }
        }
        if (candidates.empty()) {
            candidates.push_back({"pass", "total", "time"});
        }
        for (int j = i + 1; j < static_cast<int>(lines.size()) && j < i + 12; ++j) {
            const auto line = trim_ascii(lines[j]);
            if (line.empty()) {
                continue;
            }
            if (line.find("Test Summary:") != std::string::npos) {
                break;
            }
            TestSummaryResult parsed;
            bool ok = false;
            for (const auto& cols : candidates) {
                if (parse_summary_counts_row(cols, line, parsed)) {
                    ok = true;
                    break;
                }
            }
            if (ok) {
                result.found_summary = parsed.found_summary;
                result.test_pass = parsed.test_pass;
                result.test_fail = parsed.test_fail;
                result.test_broken = parsed.test_broken;
                result.test_error = parsed.test_error;
                result.test_total = parsed.test_total;
                result.test_time = parsed.test_time;
                result.status = parsed.status;
                return result;
            }
        }
    }

    return result;
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
//
// Meta.parseall parses the code as a toplevel block, so multi-statement input (e.g. a function
// definition followed by a call) is accepted; single-expression Meta.parse rejected it with
// "extra token after end of expression". parseall does not raise: it embeds Expr(:error) or
// Expr(:incomplete) nodes, so we walk the tree and report the first such node's ParseError.
Result<void> validate_julia_syntax(const std::string& code)
{
    RunSpec spec;
    spec.argv = {"julia", "--color=no", "-e",
                 "function __jjmcp_prob(x); if x isa Expr; "
                 "(x.head === :error || x.head === :incomplete) && return x; "
                 "for a in x.args; r = __jjmcp_prob(a); r === nothing || return r; end; end; "
                 "return nothing; end; "
                 "ex = Meta.parseall(read(stdin, String); filename=\"jjmcp_eval\"); "
                 "p = __jjmcp_prob(ex); "
                 "if p === nothing; print(\"ok\"); else; "
                 "a = isempty(p.args) ? string(p.head) : p.args[1]; "
                 "a isa Exception ? showerror(stderr, a) : print(stderr, a); exit(1); end"};
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

TestSummaryResult parse_test_summary(const std::string& capture)
{
    return parse_test_summary_impl(capture);
}

ToolDispatcher::ToolDispatcher(ServerState& state, const Tmux& tmux, std::filesystem::path cwd)
    : state_(state), tmux_(tmux), cwd_(std::move(cwd)),
      jobs_(state.config_path.parent_path() / "jobs")
{
}

void ToolDispatcher::shutdown()
{
    jobs_.shutdown();
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
                {"validate_syntax", {{"type", "boolean"}, {"description", "parse code via a 2s julia Meta.parseall before paste; accepts multi-statement input (default true)"}}},
                {"transport", {{"type", "string"}, {"enum", nlohmann::json::array({"auto", "tmux", "socket"})}, {"description", "auto (default), tmux (force marker path), or socket (require JJMCPHelper.jl)"}}},
                {"detach_on_timeout", {{"type", "boolean"}, {"description", "when the waiting window expires, keep polling the marker in the background and return state=running with a job_id instead of an error (default true)"}}},
            },
            nlohmann::json::array({"code"})),
        job_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_eval_async",
        "Submit Julia code to the bound REPL and return a job_id immediately. The job keeps being "
        "polled in the background, so its output survives a client timeout or reconnection. Follow "
        "with jjmcp_wait, jjmcp_job_status or jjmcp_result.",
        object_schema(
            {
                {"code", {{"type", "string"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_job_ms}, {"description", "how long the job may run before it is marked timed_out"}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
                {"force", {{"type", "boolean"}, {"description", "skip pane-mode pre-check (default false)"}}},
                {"validate_syntax", {{"type", "boolean"}, {"description", "parse code via a 2s julia Meta.parseall before paste (default true)"}}},
            },
            nlohmann::json::array({"code"})),
        job_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_wait",
        "Wait for an already-running job or marker id to finish, without resending the code. Falls "
        "back to the stored result, then to the pane scrollback, for a marker this process does not "
        "track. Returns state=running when the waiting window expires; the job keeps running.",
        object_schema(
            {
                {"job_id", {{"type", "string"}, {"description", "job id, which is the marker id"}}},
                {"marker_id", {{"type", "string"}, {"description", "alias for job_id"}}},
                {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", state_.max_timeout_ms}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            }),
        job_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_job_status",
        "Report a job's state, elapsed time, output activity, and the REPL process counters (pid, "
        "cpu seconds, RSS) read from /proc. Answers while Julia is busy. Defaults to the most recent job.",
        object_schema(
            {
                {"job_id", {{"type", "string"}}},
                {"marker_id", {{"type", "string"}, {"description", "alias for job_id"}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            }),
        job_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_result",
        "Retrieve a job's structured result after any client timeout or reconnection. Reads the "
        "in-memory job first, then the bounded on-disk result store.",
        object_schema(
            {
                {"job_id", {{"type", "string"}}},
                {"marker_id", {{"type", "string"}, {"description", "alias for job_id"}}},
                {"capture_lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            }),
        job_output_schema()));

    tools.push_back(tool_schema(
        "jjmcp_capture_job",
        "Return the output of one job by marker, instead of the last N lines of the pane. Safe for "
        "long runs: unrelated pane traffic cannot appear in the result.",
        object_schema(
            {
                {"job_id", {{"type", "string"}}},
                {"marker_id", {{"type", "string"}, {"description", "alias for job_id"}}},
                {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}},
            })));

    tools.push_back(tool_schema(
        "jjmcp_list_jobs",
        "List the jobs this jjmcp process tracks, newest last.",
        object_schema(nlohmann::json::object())));

    tools.push_back(tool_schema(
        "jjmcp_capture",
        "Capture recent output from the bound tmux pane without sending input.",
        object_schema({{"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxCaptureLines}}}})));

    tools.push_back(tool_schema(
        "jjmcp_capture_test_results",
        "Capture recent tmux output and parse the latest Julia Test Summary block.",
        object_schema(
            {
                {"lines",
                 {{"type", "integer"},
                  {"minimum", 1},
                  {"maximum", kMaxCaptureLines},
                  {"default", kDefaultCaptureLines}}},
                {"require_summary", {{"type", "boolean"},
                                   {"default", false},
                                   {"description", "fail when true and no summary is present in captured output (default false)."}},
                },
                {"include_raw", {{"type", "boolean"},
                                 {"default", true},
                                 {"description", "return raw capture text in structuredContent (default true)."}},
                },
            }),
        test_summary_output_schema()));

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
    if (name == "jjmcp_capture_test_results") {
        return capture_test_results(arguments);
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
    if (name == "jjmcp_eval_async") {
        return eval_async(arguments);
    }
    if (name == "jjmcp_wait") {
        return wait_for_job(arguments);
    }
    if (is_control_plane_tool(name)) {
        return call_control_plane(name, arguments);
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

    // Binding is the natural re-sync point: forget any earlier runtime injection for this pane so
    // the next eval installs Main.JJMCPRuntime again, whatever process now owns the pane.
    tmux_runtime_bootstrapped_.invalidate(binding.pane_id.empty() ? binding.target : binding.pane_id);

    const auto config_base = config_base_from_project(cwd_, project_root);
    set_binding(state_, std::move(binding), config_base);
    jobs_.set_dir(state_.config_path.parent_path() / "jobs");
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
    const bool detach_on_timeout = arguments.value("detach_on_timeout", true);
    return eval_code(code, timeout_ms, capture_lines, force, nullptr, transport, detach_on_timeout);
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
    return ToolResult::success(truncate_tool_tail(captured.value(), static_cast<std::size_t>(lines)));
}

ToolResult ToolDispatcher::capture_test_results(const nlohmann::json& arguments)
{
    if (!state_.binding) {
        return ToolResult::error("no tmux pane is bound; call jjmcp_bind first");
    }

    const int lines = optional_int(arguments, "lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    const bool require_summary = arguments.value("require_summary", false);
    const bool include_raw = arguments.value("include_raw", true);

    const auto captured = tmux_.capture_pane(bound_target(), lines);
    if (!captured) {
        return ToolResult::error(captured.error());
    }
    const std::string raw = truncate_tool_tail(captured.value(), static_cast<std::size_t>(lines));
    auto parsed = parse_test_summary(raw);
    parsed.test_time = trim_ascii(parsed.test_time);

    if (!parsed.found_summary) {
        parsed.status = "unknown";
    } else if (parsed.test_fail == 0 && parsed.test_broken == 0 && parsed.test_error == 0) {
        parsed.status = "pass";
    } else {
        parsed.status = "fail";
    }

    nlohmann::json structured = {
        {"found_summary", parsed.found_summary},
        {"test_pass", parsed.test_pass},
        {"test_fail", parsed.test_fail},
        {"test_broken", parsed.test_broken},
        {"test_error", parsed.test_error},
        {"test_total", parsed.test_total},
        {"test_time", parsed.test_time},
        {"status", parsed.status},
        {"failures", parsed.failures},
        {"capture_lines", lines},
    };
    if (include_raw) {
        structured["raw_output"] = raw;
    }

    if (!parsed.found_summary && require_summary) {
        return ToolResult::error("no test summary found in captured tmux output")
            .with_structured(std::move(structured));
    }

    std::string text;
    if (include_raw) {
        text = raw;
    } else if (parsed.found_summary) {
        std::ostringstream out;
        out << "Test Summary: "
            << "pass=" << parsed.test_pass << ", fail=" << parsed.test_fail
            << ", broken=" << parsed.test_broken << ", error=" << parsed.test_error
            << ", total=" << parsed.test_total;
        if (!parsed.test_time.empty()) {
            out << ", time=" << parsed.test_time;
        }
        text = out.str();
        if (!parsed.failures.empty()) {
            text += "\nfailures:";
            for (const auto& failure : parsed.failures) {
                text += "\n" + failure;
            }
        }
    } else {
        text = "No test summary found in captured output";
    }

    return ToolResult::success(std::move(text)).with_structured(std::move(structured));
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
ToolResult socket_response_to_tool_result(const SocketEvalResponse& resp, const std::string& transport_name,
                                          int capture_lines)
{
    const auto cap_lines = static_cast<std::size_t>(capture_lines);
    const std::string stdout_text = truncate_tool_tail(resp.stdout_text, cap_lines);
    const std::string value_show = truncate_tool_tail(resp.value_show, cap_lines);
    const std::string error_message = truncate_tool_tail(resp.error_message, cap_lines);
    const std::string backtrace = truncate_tool_tail(resp.backtrace, 200, kMaxMcpBacktraceBytes);
    nlohmann::json structured = {
        {"elapsed_ms", resp.elapsed_ms},
        {"timed_out", false},
        {"found_begin", true},
        {"found_end", true},
        {"julia_error", !resp.ok},
        {"marker_id", ""},
        {"stdout", stdout_text},
        {"value_repr", value_show},
        {"error_message", error_message},
        {"backtrace", backtrace},
        {"transport", transport_name},
    };
    std::string text;
    if (resp.ok) {
        text = stdout_text;
        if (!value_show.empty()) {
            if (!text.empty() && text.back() != '\n') text.push_back('\n');
            text += value_show;
        }
        text = truncate_bytes_tail(text, kMaxMcpTextBytes);
        return ToolResult::success(std::move(text)).with_structured(std::move(structured));
    }
    text = error_message;
    if (!backtrace.empty()) {
        if (!text.empty() && text.back() != '\n') text.push_back('\n');
        text += backtrace;
    }
    text = truncate_bytes_tail(text, kMaxMcpTextBytes);
    return ToolResult::error(std::move(text)).with_structured(std::move(structured));
}

} // namespace

bool RuntimeBootstrapCache::is_current(const std::string& pane_key, const std::string& generation) const
{
    const auto it = generations_.find(pane_key);
    if (it == generations_.end()) {
        return false;
    }
    if (generation.empty() || it->second.empty()) {
        return true;
    }
    return it->second == generation;
}

void RuntimeBootstrapCache::mark(const std::string& pane_key, std::string generation)
{
    generations_[pane_key] = std::move(generation);
}

void RuntimeBootstrapCache::invalidate(const std::string& pane_key)
{
    generations_.erase(pane_key);
}

std::string pane_foreground_generation(const std::string& pane_pid)
{
    // The foreground process group of the pane terminal identifies the REPL currently running
    // there, so a restarted Julia in the same pane gets a different token.
    const long long foreground = pane_foreground_pid(pane_pid);
    if (foreground <= 0) {
        return {};
    }
    return pane_pid + ":" + std::to_string(foreground);
}

Result<void> ToolDispatcher::ensure_jjmcp_runtime(const int timeout_ms, const std::string& pane_pid)
{
    if (!state_.binding) {
        return Result<void>::failure("no tmux pane is bound; call jjmcp_bind first");
    }

    const std::string pane_key = bound_target();
    const std::string generation = pane_foreground_generation(pane_pid);
    if (tmux_runtime_bootstrapped_.is_current(pane_key, generation)) {
        return Result<void>::success();
    }

    const Marker marker = make_marker(make_marker_id(++state_.marker_sequence));
    const std::string bootstrap = make_jjmcp_runtime_bootstrap_code(marker);
    const std::string buffer_name = "jjmcp_bootstrap_" + marker.id;
    const auto sent = tmux_.send_text(bound_target(), bootstrap, buffer_name);
    if (!sent) {
        return Result<void>::failure(sent.error());
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(std::max(1000, std::min(timeout_ms, 30000)));
    std::chrono::milliseconds backoff{50};
    constexpr std::chrono::milliseconds kBackoffMax{500};
    std::string accumulator;
    std::string last_capture;
    ExtractedOutput extracted;

    while (true) {
        const auto captured = tmux_.capture_pane(bound_target(), 2000);
        if (!captured) {
            return Result<void>::failure(captured.error());
        }
        const std::string& cap = captured.value();
        if (accumulator.empty()) {
            accumulator = cap;
        } else if (cap != last_capture) {
            const std::size_t overlap = compute_capture_overlap(last_capture, cap);
            if (overlap < cap.size()) {
                accumulator.append(cap, overlap, cap.size() - overlap);
            }
        }
        last_capture = cap;

        extracted = extract_between_markers(cap, marker);
        if (!extracted.found_begin) {
            extracted = extract_between_markers(accumulator, marker);
        }
        if (extracted.found_end) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::ostringstream out;
            out << "Timed out while installing JJMCP Julia runtime before " << marker.end;
            if (!last_capture.empty()) {
                out << "\nRecent pane output:\n";
                out << truncate_tool_tail(last_capture, 120);
            }
            return Result<void>::failure(out.str());
        }
        std::this_thread::sleep_for(std::min(backoff, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        backoff = std::min(backoff * 2, kBackoffMax);
    }

    if (extracted.julia_error) {
        std::string text = extracted.error_message;
        if (!extracted.backtrace.empty()) {
            if (!text.empty() && text.back() != '\n') text.push_back('\n');
            text += extracted.backtrace;
        }
        return Result<void>::failure("failed to install JJMCP Julia runtime:\n" + text);
    }

    tmux_runtime_bootstrapped_.mark(pane_key, generation);
    return Result<void>::success();
}

namespace {

// Text payload for a finished job, mirroring the pre-job behaviour: stdout plus the value repr on
// success, the showerror message plus backtrace on a Julia error.
std::string job_text(const nlohmann::json& snapshot)
{
    const std::string error_message = snapshot.value("error_message", "");
    const std::string backtrace = snapshot.value("backtrace", "");
    if (snapshot.value("julia_error", false)) {
        std::string text = error_message;
        if (!backtrace.empty()) {
            if (!text.empty() && text.back() != '\n') text.push_back('\n');
            text += backtrace;
        }
        return truncate_bytes_tail(text, kMaxMcpTextBytes);
    }
    std::string text = snapshot.value("stdout", "");
    const std::string value_repr = snapshot.value("value_repr", "");
    if (!value_repr.empty()) {
        if (!text.empty() && text.back() != '\n') text.push_back('\n');
        text += value_repr;
    }
    return truncate_bytes_tail(text, kMaxMcpTextBytes);
}

// Rebuild a ToolResult from a stored job record, so a result survives eviction from memory, a
// client reconnect, or a jjmcp restart even when tmux scrollback no longer holds the output.
ToolResult persisted_job_result(const nlohmann::json& stored)
{
    nlohmann::json structured = stored;
    structured["from_result_store"] = true;
    const std::string state = structured.value("state", "unknown");
    if (state == "completed") {
        std::string text = job_text(structured);
        ToolResult result = structured.value("julia_error", false)
                                ? ToolResult::error(std::move(text))
                                : ToolResult::success(std::move(text));
        return result.with_structured(std::move(structured));
    }

    std::ostringstream out;
    out << "Stored job " << structured.value("job_id", std::string()) << " ended as " << state;
    if (const std::string failure = structured.value("failure", ""); !failure.empty()) {
        out << ": " << failure;
    }
    if (const std::string text = structured.value("text", ""); !text.empty()) {
        out << "\nOutput:\n" << text;
    }
    return ToolResult::error(out.str()).with_structured(std::move(structured));
}

// Uniform ToolResult for any job state. `detached` reports a job that outlived its waiting window
// and is still being polled in the background.
ToolResult job_to_tool_result(const EvalJob& job, const int capture_lines)
{
    nlohmann::json snapshot = job.snapshot(capture_lines);
    const std::string state = snapshot.value("state", "unknown");

    if (state == "completed") {
        std::string text = job_text(snapshot);
        ToolResult result = snapshot.value("julia_error", false) ? ToolResult::error(std::move(text))
                                                                 : ToolResult::success(std::move(text));
        return result.with_structured(std::move(snapshot));
    }

    std::ostringstream out;
    if (state == "running") {
        out << "Still running after " << snapshot.value("elapsed_ms", 0) << " ms. The job kept its "
            << "marker and keeps being polled in the background.\n"
            << "job_id: " << job.id << "\n"
            << "Call jjmcp_wait(job_id) to keep waiting, jjmcp_job_status(job_id) for process "
            << "activity, or jjmcp_result(job_id) once it finishes.";
        const std::string tail = snapshot.value("live_tail", "");
        if (!tail.empty()) {
            out << "\nOutput so far:\n" << tail;
        }
        return ToolResult::success(out.str()).with_structured(std::move(snapshot));
    }

    if (state == "timed_out") {
        out << "Timed out after " << job.timeout_ms << " ms waiting for " << job.marker.end << "\n"
            << "job_id: " << job.id;
        const std::string partial = snapshot.value("stdout", "");
        if (snapshot.value("found_begin", false) && !partial.empty()) {
            out << "\nPartial output between markers:\n" << partial;
        }
        return ToolResult::error(out.str()).with_structured(std::move(snapshot));
    }

    out << "Job " << job.id << " failed: " << snapshot.value("failure", "unknown error");
    return ToolResult::error(out.str()).with_structured(std::move(snapshot));
}

} // namespace

Result<std::shared_ptr<EvalJob>> ToolDispatcher::send_job(const JobRequest& request,
                                                          const std::string& pane_pid,
                                                          std::unique_ptr<AdvisoryLock> lock)
{
    using JobResultT = Result<std::shared_ptr<EvalJob>>;

    auto job = std::make_shared<EvalJob>();
    job->id = next_marker_id(state_);
    job->marker = make_marker(job->id);
    job->target = bound_target();
    job->pane_pid = pane_pid;
    job->code = request.code;
    job->timeout_ms = request.job_ms;
    job->capture_lines = request.capture_lines;
    job->submitted_at = timestamp_now();
    job->started = std::chrono::steady_clock::now();
    job->lock = std::move(lock);

    // Fast single-line prints wrap into thousands of terminal rows and can push BEGIN out of a
    // small capture window before the first post-send poll. Keep the polling window larger than the
    // returned capture window; the final MCP payload is capped separately.
    const int poll_capture_lines = std::clamp(request.capture_lines + 8000, 10000, kMaxCaptureLines);
    job->poller = std::make_unique<MarkerPoller>(tmux_, job->target, job->marker, poll_capture_lines);

    const std::string buffer_name = "jjmcp_" + job->id;
    const std::string wrapped = wrap_julia_code(request.code, job->marker, request.display_ms);
    if (const auto sent = tmux_.send_text(job->target, wrapped, buffer_name); !sent) {
        return JobResultT::failure(sent.error());
    }
    if (const auto registered = jobs_.register_running(job); !registered) {
        return JobResultT::failure(registered.error());
    }
    return JobResultT::success(std::move(job));
}

Result<std::shared_ptr<EvalJob>> ToolDispatcher::start_marker_job(const JobRequest& request,
                                                                  ProgressEmitter* progress,
                                                                  PollOutcome& outcome)
{
    using JobResultT = Result<std::shared_ptr<EvalJob>>;

    if (!state_.binding) {
        return JobResultT::failure("no tmux pane is bound; call jjmcp_bind first");
    }
    const auto info = tmux_.pane_info(bound_target());
    if (!info) {
        return JobResultT::failure(info.error());
    }
    if (!info.value().alive()) {
        return JobResultT::failure("bound tmux pane appears dead: " + bound_target());
    }
    if (const auto busy = jobs_.running_in(bound_target()); busy != nullptr) {
        return JobResultT::failure("job " + busy->id + " is still running in " + bound_target()
                                   + "; wait for it with jjmcp_wait, or call jjmcp_interrupt");
    }

    // Pane-mode safety: refuse to paste into a non-julia REPL mode (pkg/help/shell). Try a single
    // backspace to exit common alternate modes, then re-check. force=true bypasses this entirely.
    if (!request.force) {
        const auto last = tmux_.capture_pane(bound_target(), 1);
        if (last && is_known_non_julia_prompt(last.value())) {
            (void)tmux_.send_key(bound_target(), "BSpace");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto recheck = tmux_.capture_pane(bound_target(), 1);
            if (!recheck || is_known_non_julia_prompt(recheck.value())) {
                return JobResultT::failure(
                    std::string("bound pane appears to be in a non-julia REPL mode "
                                "(pkg>/help?>/shell>). Last line: \"")
                    + (recheck ? recheck.value() : last.value())
                    + "\". Press backspace in the pane to return to julia>, or pass force=true.");
            }
        }
    }

    // Advisory lock against a second jjmcp process targeting the same pane. The job owns it for its
    // whole life, so a detached job still keeps the pane reserved.
    auto lock = std::make_unique<AdvisoryLock>(state_.config_path.parent_path() / "lock");
    if (const auto acquired = lock->acquire(std::to_string(state_.marker_sequence + 1)); !acquired) {
        return JobResultT::failure(acquired.error());
    }

    if (const auto runtime = ensure_jjmcp_runtime(request.job_ms, info.value().pane_pid); !runtime) {
        return JobResultT::failure(runtime.error());
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        auto sent = send_job(request, info.value().pane_pid, std::move(lock));
        if (!sent) {
            return sent;
        }
        auto job = sent.value();

        PollOptions options;
        // Only the first attempt reacts to a missing runtime, so a stale error already in the
        // scrollback cannot make the code run twice.
        options.detect_missing_runtime = attempt == 0;
        options.progress = progress;
        options.cancel = &job->cancel;
        options.on_tick = [&job](const bool grew) {
            if (grew) {
                job->publish_live();
            }
        };
        const auto window = std::min(request.foreground_ms, request.job_ms);
        outcome = job->poller->poll_until(job->started + std::chrono::milliseconds(window), options);

        if (outcome.stop != PollStop::RuntimeMissing) {
            return JobResultT::success(std::move(job));
        }

        // The Julia process was replaced after the runtime was injected, so the paste never ran.
        // Reclaim the pane lock before the job drops it, then re-inject and send once more.
        lock = std::move(job->lock);
        job->finish(JobState::Failed, "@JJMCP_COMMAND was missing; resent after reinstalling the runtime");
        jobs_.abandon(job);
        log::warn("jjmcp_runtime_reinjected", {{"target", bound_target()}});
        tmux_runtime_bootstrapped_.invalidate(bound_target());
        if (const auto runtime = ensure_jjmcp_runtime(request.job_ms, info.value().pane_pid); !runtime) {
            return JobResultT::failure(runtime.error());
        }
    }

    return JobResultT::failure("the Julia REPL in " + bound_target()
                               + " does not accept @JJMCP_COMMAND after reinstalling the runtime");
}

ToolResult ToolDispatcher::eval_code(const std::string& code, int timeout_ms, int capture_lines,
                                     bool force, ProgressEmitter* progress_override,
                                     const std::string& transport, bool detach_on_timeout)
{
    ProgressEmitter* progress = progress_override != nullptr ? progress_override : current_progress_;
    if (!state_.binding) {
        return ToolResult::error("no tmux pane is bound; call jjmcp_bind first");
    }

    // Transport routing: when transport is "socket" or "auto" and a JJMCPHelper.jl socket exists
    // for the bound pane, prefer that path for structured returns and millisecond-scale latency.
    // The socket path runs eval at top level via Base.include_string(Main, ...) and echoes
    // stdout/stderr to the live REPL, so the human-visible scrollback is unchanged. With
    // transport=tmux we always use the marker path. With transport=auto, a socket failure falls
    // back to the marker path with a warn log; with transport=socket, a failure is returned as-is.
    if (transport != "tmux") {
        const std::string& pane_id = state_.binding->pane_id.empty() ? state_.binding->target : state_.binding->pane_id;
        const std::string sock_path = SocketClient::default_socket_path(pane_id);
        if (SocketClient::socket_exists(sock_path)) {
            const SocketClient client;
            const auto resp = client.eval(sock_path, code, std::chrono::milliseconds(timeout_ms),
                                          kMaxSocketFieldBytes);
            if (resp) {
                return socket_response_to_tool_result(resp.value(), "socket", capture_lines);
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

    // timeout_ms bounds how long this call waits. A job that outlives that window is handed to the
    // background poller, so its own lifetime must be longer than the wait, not equal to it.
    JobRequest request;
    request.code = code;
    request.job_ms = detach_on_timeout
                         ? std::max(timeout_ms, std::min(kDefaultJobTimeoutMs, state_.max_job_ms))
                         : timeout_ms;
    request.display_ms = timeout_ms;
    request.capture_lines = capture_lines;
    request.foreground_ms = timeout_ms;
    request.force = force;

    PollOutcome outcome;
    auto started = start_marker_job(request, progress, outcome);
    if (!started) {
        return ToolResult::error(started.error());
    }
    auto job = started.value();

    if (outcome.stop == PollStop::Pending && detach_on_timeout) {
        // The waiting window expired but the REPL is still working. Hand the poller (and its
        // accumulator) to a background thread so no output is lost between here and jjmcp_wait.
        job->publish_live();
        jobs_.detach(job);
        return job_to_tool_result(*job, capture_lines);
    }

    finish_from_outcome(job, outcome);
    jobs_.retire(job);
    return job_to_tool_result(*job, capture_lines);
}

ToolResult ToolDispatcher::eval_async(const nlohmann::json& arguments)
{
    std::string error;
    const std::string code = require_string(arguments, "code", error);
    if (!error.empty()) {
        return ToolResult::error(error);
    }
    const int timeout_ms = optional_int(arguments, "timeout_ms",
                                        std::min(kDefaultJobTimeoutMs, state_.max_job_ms), 1,
                                        state_.max_job_ms);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);
    const bool force = arguments.value("force", false);
    const bool validate_syntax = arguments.value("validate_syntax", true);
    if (validate_syntax) {
        if (const auto ok = validate_julia_syntax(code); !ok) {
            return ToolResult::error("syntax error before eval (set validate_syntax=false to skip):\n"
                                     + ok.error());
        }
    }

    JobRequest request;
    request.code = code;
    request.job_ms = timeout_ms;
    request.display_ms = timeout_ms;
    request.capture_lines = capture_lines;
    request.foreground_ms = kJobBootstrapWindowMs;
    request.force = force;

    PollOutcome outcome;
    auto started = start_marker_job(request, nullptr, outcome);
    if (!started) {
        return ToolResult::error(started.error());
    }
    auto job = started.value();

    if (outcome.stop == PollStop::Pending) {
        job->publish_live();
        jobs_.detach(job);
    } else {
        // Short work can already be finished when the bootstrap window closes.
        finish_from_outcome(job, outcome);
        jobs_.retire(job);
    }
    return job_to_tool_result(*job, capture_lines);
}

ToolResult ToolDispatcher::wait_for_job(const nlohmann::json& arguments)
{
    const std::string job_id = optional_string(arguments, "job_id", optional_string(arguments, "marker_id"));
    if (job_id.empty()) {
        return ToolResult::error("job_id is required");
    }
    const int timeout_ms = optional_int(arguments, "timeout_ms", kDefaultWaitMs, 1, state_.max_timeout_ms);
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);

    if (const auto job = jobs_.find(job_id); job != nullptr) {
        {
            std::unique_lock<std::mutex> guard(job->mu);
            job->cv.wait_for(guard, std::chrono::milliseconds(timeout_ms),
                             [&job]() { return job->state != JobState::Running; });
        }
        return job_to_tool_result(*job, capture_lines);
    }

    if (auto persisted = jobs_.load_persisted(job_id); persisted) {
        return persisted_job_result(*persisted);
    }

    // The marker belongs to no job this process tracks: it came from an earlier jjmcp, or from a
    // synchronous eval whose result was never claimed. Rebuild the marker from its id and read the
    // pane. Whatever has already scrolled out of the tmux history is not recoverable this way.
    if (!state_.binding) {
        return ToolResult::error("no job " + job_id + " and no bound pane to recover it from");
    }
    auto job = std::make_shared<EvalJob>();
    job->id = job_id;
    job->marker = make_marker(job_id);
    job->target = bound_target();
    job->timeout_ms = timeout_ms;
    job->capture_lines = capture_lines;
    job->submitted_at = timestamp_now();
    job->started = std::chrono::steady_clock::now();
    const int poll_capture_lines = std::clamp(capture_lines + 8000, 10000, kMaxCaptureLines);
    job->poller = std::make_unique<MarkerPoller>(tmux_, job->target, job->marker, poll_capture_lines);

    PollOptions options;
    options.progress = current_progress_;
    const auto outcome = job->poller->poll_until(
        job->started + std::chrono::milliseconds(timeout_ms), options);
    finish_from_outcome(job, outcome);

    ToolResult result = job_to_tool_result(*job, capture_lines);
    result.structured["recovered_from_scrollback"] = true;
    return result;
}

ToolResult ToolDispatcher::job_status(const nlohmann::json& arguments) const
{
    const std::string job_id = optional_string(arguments, "job_id", optional_string(arguments, "marker_id"));
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultStatusLines, 1, kMaxCaptureLines);

    const auto job = job_id.empty() ? jobs_.most_recent() : jobs_.find(job_id);
    if (job == nullptr) {
        if (!job_id.empty()) {
            if (auto persisted = jobs_.load_persisted(job_id); persisted) {
                return persisted_job_result(*persisted);
            }
            return ToolResult::error("unknown job: " + job_id);
        }
        return ToolResult::error("no jobs have run in this jjmcp process");
    }

    nlohmann::json structured = job->snapshot(capture_lines);

    // Process activity is read from /proc, so it stays available while Julia itself is too busy to
    // answer. The foreground process group of the pane terminal is the REPL running there.
    const long long repl_pid = pane_foreground_pid(job->pane_pid);
    const ProcStats stats = read_proc_stats(repl_pid);
    structured["pane_pid"] = job->pane_pid;
    structured["repl_pid"] = repl_pid;
    structured["proc_available"] = stats.available;
    if (stats.available) {
        structured["proc_state"] = stats.state;
        structured["cpu_seconds"] = stats.cpu_seconds;
        structured["rss_bytes"] = stats.rss_bytes;
        structured["threads"] = stats.threads;
    }
    if (const auto info = tmux_.pane_info(job->target); info) {
        structured["pane_alive"] = info.value().alive();
        structured["pane_current_command"] = info.value().current_command;
    }

    std::ostringstream out;
    out << "job_id: " << job->id << '\n';
    out << "state: " << structured.value("state", "unknown") << '\n';
    out << "target: " << job->target << '\n';
    out << "elapsed_ms: " << structured.value("elapsed_ms", 0) << '\n';
    out << "output_bytes: " << structured.value("output_bytes", 0) << '\n';
    if (structured.contains("last_output_age_ms")) {
        out << "last_output_age_ms: " << structured.value("last_output_age_ms", 0) << '\n';
    }
    if (stats.available) {
        out << "repl_pid: " << repl_pid << " (" << stats.state << ")\n";
        out << "cpu_seconds: " << stats.cpu_seconds << '\n';
        out << "rss_bytes: " << stats.rss_bytes << '\n';
    } else {
        out << "repl process stats unavailable\n";
    }
    if (const std::string failure = structured.value("failure", ""); !failure.empty()) {
        out << "failure: " << failure << '\n';
    }
    return ToolResult::success(out.str()).with_structured(std::move(structured));
}

ToolResult ToolDispatcher::job_result(const nlohmann::json& arguments) const
{
    const std::string job_id = optional_string(arguments, "job_id", optional_string(arguments, "marker_id"));
    if (job_id.empty()) {
        return ToolResult::error("job_id is required");
    }
    const int capture_lines = optional_int(arguments, "capture_lines", kDefaultCaptureLines, 1, kMaxCaptureLines);

    if (const auto job = jobs_.find(job_id); job != nullptr) {
        return job_to_tool_result(*job, capture_lines);
    }
    if (auto persisted = jobs_.load_persisted(job_id); persisted) {
        return persisted_job_result(*persisted);
    }
    return ToolResult::error("unknown job: " + job_id
                             + " (it is not in this process and no stored result exists; "
                               "call jjmcp_wait to recover the marker from the pane)");
}

ToolResult ToolDispatcher::capture_job(const nlohmann::json& arguments) const
{
    const std::string job_id = optional_string(arguments, "job_id", optional_string(arguments, "marker_id"));
    if (job_id.empty()) {
        return ToolResult::error("job_id is required");
    }
    const int lines = optional_int(arguments, "lines", kDefaultCaptureLines, 1, kMaxCaptureLines);

    const auto job = jobs_.find(job_id);
    if (job == nullptr) {
        if (auto persisted = jobs_.load_persisted(job_id); persisted) {
            const std::string text = truncate_tool_tail(persisted->value("text", ""),
                                                        static_cast<std::size_t>(lines));
            return ToolResult::success(text).with_structured(
                {{"job_id", job_id}, {"state", persisted->value("state", "unknown")}, {"text", text}});
        }
        return ToolResult::error("unknown job: " + job_id);
    }

    std::string text;
    std::string state;
    std::size_t output_bytes = 0;
    {
        std::lock_guard<std::mutex> guard(job->mu);
        state = job_state_name(job->state);
        output_bytes = job->output_bytes;
        text = truncate_tool_tail(job->state == JobState::Running ? job->live_tail : job->result.text,
                                  static_cast<std::size_t>(lines));
    }
    return ToolResult::success(text).with_structured({
        {"job_id", job->id},
        {"marker_id", job->marker.id},
        {"state", state},
        {"target", job->target},
        {"output_bytes", output_bytes},
        {"lines", lines},
        {"text", text},
    });
}

ToolResult ToolDispatcher::list_jobs() const
{
    nlohmann::json entries = nlohmann::json::array();
    std::ostringstream out;
    for (const auto& job : jobs_.list()) {
        nlohmann::json entry = job->snapshot(1);
        entry.erase("stdout");
        entry.erase("value_repr");
        entry.erase("error_message");
        entry.erase("backtrace");
        entry.erase("live_tail");
        entry["code_preview"] = truncate_bytes_tail(truncate_lines(job->code, 3), 300);
        out << entry.value("state", "unknown") << "  " << job->id << "  " << job->target << "  "
            << entry.value("elapsed_ms", 0) << " ms\n";
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        return ToolResult::success("no jobs in this jjmcp process")
            .with_structured({{"jobs", entries}});
    }
    return ToolResult::success(out.str()).with_structured({{"jobs", std::move(entries)}});
}

bool ToolDispatcher::is_control_plane_tool(const std::string& name)
{
    return name == "jjmcp_job_status" || name == "jjmcp_result" || name == "jjmcp_capture_job"
        || name == "jjmcp_list_jobs";
}

ToolResult ToolDispatcher::call_control_plane(const std::string& name,
                                              const nlohmann::json& arguments) const
{
    if (name == "jjmcp_job_status") {
        return job_status(arguments);
    }
    if (name == "jjmcp_result") {
        return job_result(arguments);
    }
    if (name == "jjmcp_capture_job") {
        return capture_job(arguments);
    }
    if (name == "jjmcp_list_jobs") {
        return list_jobs();
    }
    return ToolResult::error("unknown tool: " + name);
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
