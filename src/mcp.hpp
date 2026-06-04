#pragma once

#include "result.hpp"

#include <iosfwd>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace jjmcp {

class ToolDispatcher;

struct ReadMessage {
    bool eof = false;
    std::string body;
};

Result<ReadMessage> read_mcp_message(std::istream& input);
void write_mcp_message(std::ostream& output, const nlohmann::json& message);

nlohmann::json make_jsonrpc_result(const nlohmann::json& id, nlohmann::json result);
nlohmann::json make_jsonrpc_error(const nlohmann::json& id, int code, const std::string& message);
// stdout_mutex serializes all writes to the output stream across the read thread and the worker
// thread (final responses, and progress frames emitted from inside a running tool call).
std::optional<nlohmann::json> dispatch_mcp_request(const nlohmann::json& request, ToolDispatcher& tools,
                                                   std::mutex& stdout_mutex);

} // namespace jjmcp
