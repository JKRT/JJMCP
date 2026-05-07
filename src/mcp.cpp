#include "mcp.hpp"

#include "tools.hpp"

#include <iostream>
#include <sstream>

namespace jjmcp {
namespace {

nlohmann::json request_id_or_null(const nlohmann::json& request)
{
    if (request.is_object() && request.contains("id")) {
        return request["id"];
    }
    return nullptr;
}

bool is_notification(const nlohmann::json& request)
{
    return request.is_object() && !request.contains("id");
}

nlohmann::json tool_result_json(const ToolResult& result)
{
    nlohmann::json payload;
    payload["content"] = nlohmann::json::array({{{"type", "text"}, {"text", result.text}}});
    if (result.is_error) {
        payload["isError"] = true;
    }
    return payload;
}

} // namespace

// MCP stdio transport per spec: messages are newline-delimited JSON-RPC,
// one JSON object per line, with no Content-Length headers. Embedded
// newlines inside the JSON body are forbidden (and nlohmann::json::dump()
// with no indent argument already produces single-line output, so encoding
// reduces to a single trailing '\n'). Tolerate CRLF on input and skip blank
// lines defensively.
//
// Reference: https://spec.modelcontextprotocol.io/specification/basic/transports/#stdio
Result<ReadMessage> read_mcp_message(std::istream& input)
{
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;  // skip blank separators
        }
        return Result<ReadMessage>::success(ReadMessage{false, std::move(line)});
    }
    return Result<ReadMessage>::success(ReadMessage{true, {}});
}

void write_mcp_message(std::ostream& output, const nlohmann::json& message)
{
    output << message.dump() << '\n';
    output.flush();
}

nlohmann::json make_jsonrpc_result(const nlohmann::json& id, nlohmann::json result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json make_jsonrpc_error(const nlohmann::json& id, int code, const std::string& message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}},
    };
}

std::optional<nlohmann::json> dispatch_mcp_request(const nlohmann::json& request, ToolDispatcher& tools)
{
    const auto id = request_id_or_null(request);

    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method") || !request["method"].is_string()) {
        return make_jsonrpc_error(id, -32600, "Invalid Request");
    }

    const std::string method = request["method"];
    const bool notification = is_notification(request);

    if (method == "notifications/initialized") {
        return std::nullopt;
    }

    if (notification) {
        return std::nullopt;
    }

    if (method == "initialize") {
        nlohmann::json result;
        result["protocolVersion"] = "2024-11-05";
        result["capabilities"] = {{"tools", nlohmann::json::object()}};
        result["serverInfo"] = {{"name", "JohnJuliaMCP"}, {"version", "0.1.0"}};
        return make_jsonrpc_result(id, std::move(result));
    }

    if (method == "ping") {
        return make_jsonrpc_result(id, nlohmann::json::object());
    }

    if (method == "tools/list") {
        return make_jsonrpc_result(id, {{"tools", tools.list_tools_json()}});
    }

    if (method == "tools/call") {
        if (!request.contains("params") || !request["params"].is_object()) {
            return make_jsonrpc_error(id, -32602, "tools/call params must be an object");
        }
        const auto& params = request["params"];
        if (!params.contains("name") || !params["name"].is_string()) {
            return make_jsonrpc_error(id, -32602, "tools/call params.name must be a string");
        }
        nlohmann::json arguments = nlohmann::json::object();
        if (params.contains("arguments")) {
            if (!params["arguments"].is_object()) {
                return make_jsonrpc_error(id, -32602, "tools/call params.arguments must be an object");
            }
            arguments = params["arguments"];
        }

        const auto result = tools.call(params["name"], arguments);
        return make_jsonrpc_result(id, tool_result_json(result));
    }

    return make_jsonrpc_error(id, -32601, "Method not found");
}

} // namespace jjmcp
