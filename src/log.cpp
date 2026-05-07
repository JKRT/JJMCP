#include "log.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace jjmcp::log {

namespace {

bool json_format_active()
{
    static const bool active = []() {
        const char* env = std::getenv("JJMCP_LOG_FORMAT");
        return env != nullptr && std::string{env} == "json";
    }();
    return active;
}

std::string current_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
    return out.str();
}

void emit(const std::string& level, const std::string& event, nlohmann::json fields)
{
    static std::mutex stderr_mutex;
    std::lock_guard<std::mutex> guard(stderr_mutex);
    if (json_format_active()) {
        nlohmann::json doc = {
            {"ts", current_timestamp()},
            {"level", level},
            {"event", event},
        };
        if (fields.is_object()) {
            doc.update(fields);
        }
        std::cerr << doc.dump() << '\n';
    } else {
        std::cerr << current_timestamp() << ' ' << level << ' ' << event;
        if (fields.is_object() && !fields.empty()) {
            for (const auto& [k, v] : fields.items()) {
                std::cerr << ' ' << k << '=';
                if (v.is_string()) {
                    std::cerr << v.get<std::string>();
                } else {
                    std::cerr << v.dump();
                }
            }
        }
        std::cerr << '\n';
    }
}

} // namespace

void info(const std::string& event, nlohmann::json fields)
{
    emit("INFO", event, std::move(fields));
}

void warn(const std::string& event, nlohmann::json fields)
{
    emit("WARN", event, std::move(fields));
}

void error(const std::string& event, nlohmann::json fields)
{
    emit("ERROR", event, std::move(fields));
}

} // namespace jjmcp::log
