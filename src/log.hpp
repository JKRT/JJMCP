#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace jjmcp::log {

// Structured stderr logger. Output format selected by JJMCP_LOG_FORMAT env var:
//   text (default): "<timestamp> <LEVEL> <event> key=value key=value ..."
//   json:           one JSON object per line with ts/level/event plus fields
//
// stdout is reserved for MCP frames; stderr only ever receives these log lines plus any
// uncaught C++/Julia diagnostics from underlying tooling. Calls are serialized through a
// mutex so concurrent emissions cannot interleave at the byte level.
void info(const std::string& event, nlohmann::json fields = nlohmann::json::object());
void warn(const std::string& event, nlohmann::json fields = nlohmann::json::object());
void error(const std::string& event, nlohmann::json fields = nlohmann::json::object());

} // namespace jjmcp::log
