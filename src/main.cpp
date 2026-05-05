#include "mcp.hpp"
#include "state.hpp"
#include "tmux.hpp"
#include "tools.hpp"

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
        << "  serve   Run MCP JSON-RPC over stdio. stdout is reserved for MCP frames.\n";
}

int serve()
{
    using namespace jjmcp;

    std::ios::sync_with_stdio(false);

    ServerState state;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    state.config_path = default_config_path(ec ? std::filesystem::path(".") : cwd);
    if (const auto loaded = load_config(state); !loaded) {
        std::cerr << "jjmcp: warning: " << loaded.error() << '\n';
    }

    const Tmux tmux;
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
