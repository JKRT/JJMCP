#pragma once

#include <iosfwd>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace jjmcp {

// ProgressEmitter wraps the optional MCP progressToken from a request's params._meta.
// When a token was supplied, emit() writes a `notifications/progress` JSON-RPC frame to the bound
// output stream (typically stdout) under a mutex so concurrent emissions cannot interleave at the
// byte level. When the token is null, emit() is a no-op so callers can hold a single reference and
// not branch.
class ProgressEmitter {
public:
    ProgressEmitter(std::ostream& output, nlohmann::json token);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] const nlohmann::json& token() const { return token_; }

    // Emit a progress notification. progress is monotonic; message is the new tail since the last
    // notification (may be empty). total is optional and omitted when unspecified (-1).
    void emit(double progress, const std::string& message, double total = -1.0);

private:
    std::ostream& output_;
    nlohmann::json token_;
    std::mutex mutex_;
    bool active_;
};

} // namespace jjmcp
