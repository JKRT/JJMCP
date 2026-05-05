#pragma once

#include "result.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace jjmcp {

struct Binding {
    std::string target;
    std::string pane_id;
    std::string project_root;
    std::string bound_at;
};

struct ServerState {
    std::optional<Binding> binding;
    std::filesystem::path config_path;
    std::string last_marker;
    std::string last_command_timestamp;
    unsigned long long marker_sequence = 0;
};

std::filesystem::path default_config_path(const std::filesystem::path& base);
std::string timestamp_now();
std::string next_marker_id(ServerState& state);

Result<void> load_config(ServerState& state);
Result<void> save_config(const ServerState& state);
void set_binding(ServerState& state, Binding binding, const std::filesystem::path& base_for_config);

} // namespace jjmcp
