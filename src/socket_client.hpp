#pragma once

#include "result.hpp"

#include <chrono>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

namespace jjmcp {

struct SocketEvalResponse {
    bool ok = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string value_show;
    std::string error_message;
    std::string backtrace;
    long long elapsed_ms = 0;
};

// Talks to JJMCPHelper.jl over a Unix domain socket. The helper runs eval at top level via
// Base.include_string(Main, ...) and echoes stdout/stderr back to the live REPL, so the human keeps the
// shared-REPL property while the agent receives a structured copy here.
class SocketClient {
public:
    // Mirror of JJMCPHelper.default_socket_path: $XDG_RUNTIME_DIR/jjmcp-<pane_no_percent>.sock.
    // Falls back to /tmp when XDG_RUNTIME_DIR is unset and to "default" when pane_id is empty.
    static std::string default_socket_path(const std::string& pane_id);

    // Returns true iff the path exists in the filesystem AND is a Unix socket.
    static bool socket_exists(const std::string& path);

    // Connect, send {"op":"eval","code":...}, read one JSON-line response, close. The send/recv
    // calls are bounded by `timeout` via SO_SNDTIMEO / SO_RCVTIMEO.
    Result<SocketEvalResponse> eval(const std::string& path,
                                    const std::string& code,
                                    std::chrono::milliseconds timeout,
                                    std::size_t max_output_bytes) const;
};

} // namespace jjmcp
