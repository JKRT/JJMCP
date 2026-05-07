#include "log.hpp"
#include "mcp.hpp"
#include "state.hpp"
#include "tmux.hpp"
#include "tools.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {

void print_usage()
{
    std::cout
        << "JohnJuliaMCP (jjmcp)\n"
        << "\n"
        << "Usage:\n"
        << "  jjmcp --help\n"
        << "  jjmcp serve\n"
        << "\n"
        << "Commands:\n"
        << "  serve   Run MCP JSON-RPC over stdio. stdout is reserved for MCP frames.\n"
        << "\n"
        << "Environment:\n"
        << "  JJMCP_TIMEOUT_MS_MAX  Override the per-tool timeout_ms upper bound (default 600000).\n"
        << "  JJMCP_LOG_FORMAT      Set to 'json' for one-JSON-object-per-line stderr logs (default 'text').\n";
}

void apply_env_overrides(jjmcp::ServerState& state)
{
    if (const char* env = std::getenv("JJMCP_TIMEOUT_MS_MAX")) {
        try {
            const int parsed = std::stoi(env);
            if (parsed > 0) {
                state.max_timeout_ms = parsed;
            } else {
                jjmcp::log::warn("env_override_ignored",
                                 {{"name", "JJMCP_TIMEOUT_MS_MAX"}, {"value", env}, {"reason", "must be a positive integer"}});
            }
        } catch (const std::exception&) {
            jjmcp::log::warn("env_override_ignored",
                             {{"name", "JJMCP_TIMEOUT_MS_MAX"}, {"value", env}, {"reason", "could not parse as integer"}});
        }
    }
}

int serve()
{
    using namespace jjmcp;

    std::ios::sync_with_stdio(false);

    ServerState state;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    state.config_path = default_config_path(ec ? std::filesystem::path(".") : cwd);
    apply_env_overrides(state);
    if (const auto loaded = load_config(state); !loaded) {
        log::warn("config_load_failed", {{"error", loaded.error()}});
    }

    const Tmux tmux;
    if (state.binding) {
        const auto info = tmux.pane_info(state.binding->pane_id.empty() ? state.binding->target : state.binding->pane_id);
        if (!info || !info.value().alive()) {
            log::warn("stale_binding_cleared",
                      {{"target", state.binding->target},
                       {"pane_id", state.binding->pane_id},
                       {"reason", info ? "pane is dead" : info.error()}});
            state.binding.reset();
        }
    }
    log::info("server_started",
              {{"config_path", state.config_path.string()},
               {"max_timeout_ms", state.max_timeout_ms},
               {"bound", state.binding.has_value()}});

    ToolDispatcher tools(state, tmux, ec ? std::filesystem::path(".") : cwd);

    for (;;) {
        const auto message = read_mcp_message(std::cin);
        if (!message) {
            write_mcp_message(std::cout, make_jsonrpc_error(nullptr, -32700, message.error()));
            if (!std::cin) {
                return 1;
            }
            continue;
        }
        if (message.value().eof) {
            return 0;
        }

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(message.value().body);
        } catch (const std::exception& e) {
            write_mcp_message(std::cout, make_jsonrpc_error(nullptr, -32700, std::string("Parse error: ") + e.what()));
            continue;
        }

        const auto response = dispatch_mcp_request(request, tools);
        if (response) {
            write_mcp_message(std::cout, *response);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage();
        return 0;
    }

    if (argc == 2 && std::string(argv[1]) == "serve") {
        return serve();
    }

    print_usage();
    return argc == 1 ? 0 : 2;
}
