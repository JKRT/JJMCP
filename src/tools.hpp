#pragma once

#include "progress.hpp"
#include "socket_client.hpp"
#include "state.hpp"
#include "tmux.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

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
    std::string bound_target() const;

    ServerState& state_;
    const Tmux& tmux_;
    std::filesystem::path cwd_;
    // Set by call() for the duration of one tool dispatch and read by eval_code() to emit
    // progress notifications. Null if the client did not pass a progressToken.
    ProgressEmitter* current_progress_ = nullptr;
};

} // namespace jjmcp
