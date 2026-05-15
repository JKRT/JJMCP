# JohnJuliaMCP (JJMCP)

JohnJuliaMCP is a local C++23 stdio MCP server for working through an existing tmux-based Julia REPL workflow. It binds to a tmux pane selected by the user, sends Julia code into that pane, and treats the visible REPL as the source of truth.

JJMCP does not spawn hidden Julia workers by default.

## Requirements

- Linux or macOS
- A C++23 compiler
- `make`
- `pkg-config`
- `nlohmann_json`
- `tmux` on `PATH`
- A tmux pane already running Julia for evaluation tools

## Build

```sh
make check-deps
make
make test
```

The server binary is written to:

```sh
build/jjmcp
```

Direct usage:

```sh
build/jjmcp --help
build/jjmcp serve
```

`serve` runs MCP JSON-RPC over stdio. During MCP mode, stdout is reserved for MCP frames only; diagnostics go to stderr.

## Install

```sh
make install PREFIX=/usr/local
```

After install, use the installed command:

```sh
jjmcp --help
jjmcp serve
```

If `$(PREFIX)/bin` is not on `PATH`, use the full installed path instead:

```sh
/usr/local/bin/jjmcp serve
```

Staged/package install:

```sh
make install DESTDIR=/tmp/jjmcp-package PREFIX=/usr
```

A staged install writes files under `$(DESTDIR)$(PREFIX)` for packaging. It does not make `/tmp/jjmcp-package/usr/bin/jjmcp` the normal runtime path unless you explicitly run that staged binary. After package installation, the runtime command path would be `/usr/bin/jjmcp`.

Installed files:

- `$(PREFIX)/bin/jjmcp`
- `$(PREFIX)/share/man/man1/jjmcp.1`
- `$(PREFIX)/share/man/man5/jjmcp-config.5`
- `$(PREFIX)/share/man/man7/jjmcp-security.7`
- `$(PREFIX)/share/doc/jjmcp/README.md`
- `$(PREFIX)/share/doc/jjmcp/SECURITY.md`
- `$(PREFIX)/share/doc/jjmcp/DESIGN.md`

## MCP Client Examples

Use an absolute path to the executable. If you have run `make install PREFIX=/usr/local`, use `/usr/local/bin/jjmcp`. If you have not installed JJMCP, use the absolute path to `build/jjmcp` in your checkout.

Codex CLI style TOML:

```toml
[mcp_servers.jjmcp]
command = "/usr/local/bin/jjmcp"
args = ["serve"]
```

Claude Code style JSON:

```json
{
  "mcpServers": {
    "jjmcp": {
      "command": "/usr/local/bin/jjmcp",
      "args": ["serve"]
    }
  }
}
```

Claude Code command form:

```sh
claude mcp add jjmcp /usr/local/bin/jjmcp serve
```

## tmux Workflow

Start Julia in tmux:

```sh
tmux new-session -s julia
julia --project=.
```

From an MCP client:

1. Call `jjmcp_list_tmux` to find the Julia pane id, such as `%3`.
2. Call `jjmcp_bind` with `{"target":"%3","project_root":"/path/to/project"}`.
3. Call `jjmcp_eval` with Julia code, for example `{"code":"1 + 1"}`.

Bindings are stored in memory and persisted to `.jjmcp/config.json` under the supplied `project_root`, or under the server current directory if no project root is supplied.

## Tools

- `jjmcp_list_tmux`: list sessions, windows, panes, pane ids, process names, titles, and paths.
- `jjmcp_bind`: bind to an existing tmux pane.
- `jjmcp_status`: report binding and pane liveness.
- `jjmcp_eval`: evaluate Julia code in the bound pane using unique begin/end markers.
- `jjmcp_capture`: capture recent output without sending input.
- `jjmcp_interrupt`: send Ctrl-C to the bound pane.
- `jjmcp_revise`: run `using Revise; Revise.revise()`.
- `jjmcp_activate`: run `using Pkg; Pkg.activate(path)`.
- `jjmcp_test`: run `test_expr`, include a test file, or run `Pkg.test()`.
- `jjmcp_capture_test_results`: parse the latest Julia `Test Summary:` block from tmux output and return
  structured fields like `found_summary`, `test_pass`, `test_fail`, `test_total`, `status`, and `failures`.
- `require_summary` defaults to `false`; when set to `true`, missing summaries return a tool error. `include_raw`
  defaults to `true` and includes captured pane output in `raw_output`.

## Notes

The server sends code by loading a tmux buffer and pasting it into the bound pane. Evaluation output is isolated by unique marker lines and returned as MCP text content. Julia exceptions are captured as tool errors with the Julia error text.
