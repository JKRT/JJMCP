#pragma once

#include "jobs.hpp"
#include "progress.hpp"
#include "socket_client.hpp"
#include "state.hpp"
#include "tmux.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace jjmcp {

struct ToolResult {
    bool is_error = false;
    std::string text;
    // Optional structured payload. Emitted as MCP `structuredContent` when not null.
    // Default-constructed nlohmann::json is null; use with_structured() to attach.
    nlohmann::json structured;

    static ToolResult success(std::string text) { return ToolResult{false, std::move(text), {}}; }
    static ToolResult error(std::string text) { return ToolResult{true, std::move(text), {}}; }

    ToolResult& with_structured(nlohmann::json s)
    {
        structured = std::move(s);
        return *this;
    }
};

struct TestSummaryResult {
    bool found_summary = false;
    int test_pass = 0;
    int test_fail = 0;
    int test_broken = 0;
    int test_error = 0;
    int test_total = 0;
    std::string test_time;
    std::string status = "unknown";
    std::vector<std::string> failures;
};

TestSummaryResult parse_test_summary(const std::string& capture);

// Tracks the panes that already carry the Main.JJMCPRuntime module. The generation token names the
// process that currently owns the pane foreground, so a REPL that exited and was restarted in the
// same pane no longer matches its cached entry and gets the runtime injected again. An empty token
// means the generation could not be determined; the pane key alone then decides.
class RuntimeBootstrapCache {
public:
    [[nodiscard]] bool is_current(const std::string& pane_key, const std::string& generation) const;
    void mark(const std::string& pane_key, std::string generation);
    void invalidate(const std::string& pane_key);

private:
    std::unordered_map<std::string, std::string> generations_;
};

// Generation token for a pane, derived from the foreground process group of the pane process.
// Empty when it cannot be read.
std::string pane_foreground_generation(const std::string& pane_pid);

// One submission to the bound REPL. The two timeouts are deliberately separate: job_ms bounds the
// job itself, foreground_ms bounds only how long the calling thread waits before handing the job to
// the background poller, and display_ms is the number pasted into the pane for the human to read.
struct JobRequest {
    std::string code;
    int job_ms = 0;
    int display_ms = 0;
    int capture_lines = 0;
    int foreground_ms = 0;
    bool force = false;
};

class ToolDispatcher {
public:
    ToolDispatcher(ServerState& state, const Tmux& tmux, std::filesystem::path cwd);

    nlohmann::json list_tools_json() const;
    ToolResult call(const std::string& name, const nlohmann::json& arguments,
                    ProgressEmitter* progress = nullptr);

    // Job tools that answer from the job store alone. They never touch the binding, the marker
    // sequence or the runtime cache, so the read thread can serve them while the worker thread sits
    // inside a long evaluation. That is what keeps process activity observable while Julia is busy.
    static bool is_control_plane_tool(const std::string& name);
    ToolResult call_control_plane(const std::string& name, const nlohmann::json& arguments) const;

    // Cancels every background poller and joins its thread.
    void shutdown();

    // Resources: read-only views over the bound project's Project.toml and Manifest.toml. URIs are
    // of the form jjmcp://project/<file>. Returns an empty list if no pane is bound or the project
    // root does not contain the file.
    nlohmann::json list_resources_json() const;
    Result<nlohmann::json> read_resource(const std::string& uri) const;

private:
    ToolResult list_tmux() const;
    ToolResult bind(const nlohmann::json& arguments);
    ToolResult status() const;
    ToolResult capture_test_results(const nlohmann::json& arguments);
    ToolResult eval(const nlohmann::json& arguments);
    ToolResult capture(const nlohmann::json& arguments) const;
    ToolResult interrupt() const;
    ToolResult revise(const nlohmann::json& arguments);
    ToolResult activate(const nlohmann::json& arguments);
    ToolResult test(const nlohmann::json& arguments);
    ToolResult pkg_status(const nlohmann::json& arguments);
    ToolResult eval_async(const nlohmann::json& arguments);
    ToolResult wait_for_job(const nlohmann::json& arguments);
    ToolResult job_status(const nlohmann::json& arguments) const;
    ToolResult job_result(const nlohmann::json& arguments) const;
    ToolResult capture_job(const nlohmann::json& arguments) const;
    ToolResult list_jobs() const;

    ToolResult eval_code(const std::string& code, int timeout_ms, int capture_lines,
                         bool force = false, ProgressEmitter* progress = nullptr,
                         const std::string& transport = "auto", bool detach_on_timeout = true);
    // Paste the wrapper and register the resulting job. Takes ownership of the pane lock.
    Result<std::shared_ptr<EvalJob>> send_job(const JobRequest& request, const std::string& pane_pid,
                                              std::unique_ptr<AdvisoryLock> lock);
    // Full submit path: pane checks, pane lock, runtime bootstrap, paste, and a foreground poll of
    // at most request.foreground_ms. `outcome` reports why that poll stopped.
    Result<std::shared_ptr<EvalJob>> start_marker_job(const JobRequest& request,
                                                      ProgressEmitter* progress,
                                                      PollOutcome& outcome);
    Result<void> ensure_jjmcp_runtime(int timeout_ms, const std::string& pane_pid);
    std::string bound_target() const;

    ServerState& state_;
    const Tmux& tmux_;
    std::filesystem::path cwd_;
    // Set by call() for the duration of one tool dispatch and read by eval_code() to emit
    // progress notifications. Null if the client did not pass a progressToken.
    ProgressEmitter* current_progress_ = nullptr;
    RuntimeBootstrapCache tmux_runtime_bootstrapped_;
    JobStore jobs_;
};

} // namespace jjmcp
