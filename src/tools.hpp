#pragma once

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

class ToolDispatcher {
public:
    ToolDispatcher(ServerState& state, const Tmux& tmux, std::filesystem::path cwd);

    nlohmann::json list_tools_json() const;
    ToolResult call(const std::string& name, const nlohmann::json& arguments,
                    ProgressEmitter* progress = nullptr);

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

    ToolResult eval_code(const std::string& code, int timeout_ms, int capture_lines,
                         bool force = false, ProgressEmitter* progress = nullptr,
                         const std::string& transport = "auto");
    Result<void> ensure_jjmcp_runtime(int timeout_ms, const std::string& pane_pid);
    std::string bound_target() const;

    ServerState& state_;
    const Tmux& tmux_;
    std::filesystem::path cwd_;
    // Set by call() for the duration of one tool dispatch and read by eval_code() to emit
    // progress notifications. Null if the client did not pass a progressToken.
    ProgressEmitter* current_progress_ = nullptr;
    RuntimeBootstrapCache tmux_runtime_bootstrapped_;
};

} // namespace jjmcp
