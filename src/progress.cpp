#include "progress.hpp"

#include "mcp.hpp"

#include <ostream>
#include <utility>

namespace jjmcp {

ProgressEmitter::ProgressEmitter(std::ostream& output, std::mutex& output_mutex, nlohmann::json token)
    : output_(output), mutex_(output_mutex), token_(std::move(token)), active_(!token_.is_null())
{
}

void ProgressEmitter::emit(double progress, const std::string& message, double total)
{
    if (!active_) return;
    std::lock_guard<std::mutex> guard(mutex_);
    nlohmann::json params = {
        {"progressToken", token_},
        {"progress", progress},
    };
    if (total >= 0.0) {
        params["total"] = total;
    }
    if (!message.empty()) {
        params["message"] = message;
    }
    nlohmann::json frame = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/progress"},
        {"params", std::move(params)},
    };
    output_ << encode_mcp_frame(frame) << '\n';
    output_.flush();
}

} // namespace jjmcp
