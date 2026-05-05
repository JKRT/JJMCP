#include "state.hpp"

#include "julia_wrap.hpp"

#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace jjmcp {

std::filesystem::path default_config_path(const std::filesystem::path& base)
{
    return base / ".jjmcp" / "config.json";
}

std::string timestamp_now()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(__APPLE__)
    localtime_r(&tt, &tm);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
    return out.str();
}

std::string next_marker_id(ServerState& state)
{
    ++state.marker_sequence;
    const auto id = make_marker_id(state.marker_sequence);
    state.last_marker = id;
    state.last_command_timestamp = timestamp_now();
    return id;
}

Result<void> load_config(ServerState& state)
{
    std::error_code ec;
    if (!std::filesystem::exists(state.config_path, ec)) {
        return Result<void>::success();
    }

    std::ifstream input(state.config_path);
    if (!input) {
        return Result<void>::failure("could not open config file: " + state.config_path.string());
    }

    try {
        nlohmann::json doc;
        input >> doc;
        if (!doc.contains("binding") || !doc["binding"].is_object()) {
            return Result<void>::success();
        }
        const auto& b = doc["binding"];
        Binding binding;
        binding.target = b.value("target", "");
        binding.pane_id = b.value("pane_id", "");
        binding.project_root = b.value("project_root", "");
        binding.bound_at = b.value("bound_at", "");
        if (!binding.pane_id.empty()) {
            state.binding = std::move(binding);
        }
    } catch (const std::exception& e) {
        return Result<void>::failure(std::string("could not parse config file: ") + e.what());
    }

    return Result<void>::success();
}

Result<void> save_config(const ServerState& state)
{
    if (!state.binding) {
        return Result<void>::success();
    }

    std::error_code ec;
    std::filesystem::create_directories(state.config_path.parent_path(), ec);
    if (ec) {
        return Result<void>::failure("could not create config directory: " + ec.message());
    }

    nlohmann::json doc;
    doc["binding"] = {
        {"target", state.binding->target},
        {"pane_id", state.binding->pane_id},
        {"project_root", state.binding->project_root},
        {"bound_at", state.binding->bound_at},
    };

    std::ofstream output(state.config_path);
    if (!output) {
        return Result<void>::failure("could not write config file: " + state.config_path.string());
    }
    output << doc.dump(2) << '\n';
    return Result<void>::success();
}

void set_binding(ServerState& state, Binding binding, const std::filesystem::path& base_for_config)
{
    state.binding = std::move(binding);
    state.config_path = default_config_path(base_for_config);
}

} // namespace jjmcp
