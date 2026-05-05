#pragma once

#include "state.hpp"
#include "tmux.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace jjmcp {

struct ToolResult {
    bool is_error = false;
    std::string text;

    static ToolResult success(std::string text) { return ToolResult{false, std::move(text)}; }
    static ToolResult error(std::string text) { return ToolResult{true, std::move(text)}; }
};

class ToolDispatcher {
public:
    ToolDispatcher(ServerState& state, const Tmux& tmux, std::filesystem::path cwd);

    nlohmann::json list_tools_json() const;
    ToolResult call(const std::string& name, const nlohmann::json& arguments);

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

    ToolResult eval_code(const std::string& code, int timeout_ms, int capture_lines);
    std::string bound_target() const;

    ServerState& state_;
    const Tmux& tmux_;
    std::filesystem::path cwd_;
};

} // namespace jjmcp
