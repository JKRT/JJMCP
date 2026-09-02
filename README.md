# JohnJuliaMCP (JJMCP)

JJMCP is a local C++23 stdio MCP server for working through an existing tmux-based Julia REPL workflow. It binds to a tmux pane selected by the user, sends Julia code into that pane, and treats the visible REPL as the source of truth.
JJMCP does not spawn hidden Julia workers by default! The goal of this project is to provide a useable yet simple mcp server for AI agents when working on Julia development while at the same time keeping it simple enough such that a human can clearly see what is going on.

JJMCP is designed to work in tmux session together with a revise based Julia workflow for Julia power users who are running several active Julia processes together with local or cloud based AI agents.

The goal of this project is to support Unix systems such as macOS and Linux or Windows if the user works inside WSL.

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

Two environment variables bound how long work may run. `JJMCP_TIMEOUT_MS_MAX` caps a tool's
`timeout_ms`, which is how long one MCP call waits (default 600000). `JJMCP_JOB_MS_MAX` caps a
detached job's lifetime, which blocks no MCP call and is therefore bounded far more loosely
(default 86400000).

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
4. For work that runs longer than a client is willing to wait, call `jjmcp_eval_async` and follow it
   with `jjmcp_wait`.

Bindings are stored in memory and persisted to `.jjmcp/config.json` under the supplied `project_root`, or under the server current directory if no project root is supplied.

## Visible Julia Commands

Eval-style tools sent through the tmux transport are pasted as clean Julia macro calls after a
one-time `JJMCPRuntime` bootstrap in the bound REPL. The bootstrap defines `@JJMCP_COMMAND`; later
commands are visible and reusable instead of being large generated wrapper functions.

For example, `jjmcp_eval` with `{"code":"x = 41\nx + 1","timeout_ms":10000}` appears in the REPL as:

```julia
@JJMCP_COMMAND "<marker-id>" 10000 begin
x = 41
x + 1
end
```

The same macro path is used by all eval-style tmux tools:

```julia
@JJMCP_COMMAND "<marker-id>" 10000 begin
try
    using Revise
    Revise.revise()
    println("Revise complete")
catch e
    showerror(stdout, e, catch_backtrace())
    println()
end
end
```

```julia
@JJMCP_COMMAND "<marker-id>" 10000 begin
using Pkg
Pkg.activate("/path/to/project")
end
```

```julia
@JJMCP_COMMAND "<marker-id>" 120000 begin
using Pkg
Pkg.test()
end
```

```julia
@JJMCP_COMMAND "<marker-id>" 10000 begin
using Pkg
Pkg.status()
end
```

Inside the macro, JJMCP still emits the same private begin/end/error/value markers used for MCP
response extraction. User statements are evaluated in the caller module so globals, imports, package
state, and Revise state remain part of the bound Julia session. Non-eval MCP tools such as
`jjmcp_bind`, `jjmcp_status`, `jjmcp_capture`, and `jjmcp_interrupt` do not paste Julia code and
therefore do not appear as `@JJMCP_COMMAND` calls.

## Asynchronous, Recoverable Evaluation

A long Julia command outlives the MCP call that started it. JJMCP tracks it as a **job** so the
result is never lost when a client times out or reconnects.

The job id is the marker id, and everything is addressed by it:

```
job = jjmcp_eval_async({"code": "run_long_benchmark()"})
jjmcp_wait({"job_id": job.job_id, "timeout_ms": 60000})
jjmcp_job_status({"job_id": job.job_id})
jjmcp_result({"job_id": job.job_id})
```

Three properties make this recoverable:

1. A background thread keeps polling the marker and owns the capture accumulator, so output that
   scrolls out of the tmux pane is still held by JJMCP.
2. `jjmcp_job_status`, `jjmcp_result`, `jjmcp_capture_job`, and `jjmcp_list_jobs` are answered from
   the job store on the read thread. They report process activity while Julia is busy and while the
   worker thread is inside another evaluation.
3. Completed results are written to `.jjmcp/jobs/<job_id>.json`, bounded to the newest 32 records, so
   a result survives eviction, a client reconnect, and a jjmcp restart.

`jjmcp_eval` no longer fails when only its waiting window expires. By default it hands the run to the
background poller and returns `state: "running"` with a `job_id`, which you then pass to
`jjmcp_wait`. Set `detach_on_timeout: false` for the old error-on-timeout behaviour.

`jjmcp_wait` also accepts a marker id that this process never tracked, including one from an earlier
jjmcp. It reads the stored result first, then falls back to the pane scrollback and reports
`recovered_from_scrollback: true`. Scrollback recovery can only return what tmux still holds.

One job runs per pane at a time. A second submit is refused while a job is in flight.

## Tools

- `jjmcp_list_tmux`: list sessions, windows, panes, pane ids, process names, titles, and paths.
- `jjmcp_bind`: bind to an existing tmux pane.
- `jjmcp_status`: report binding and pane liveness.
- `jjmcp_eval`: evaluate Julia code in the bound pane using `@JJMCP_COMMAND`. On timeout it detaches
  the run and returns `state: "running"` with a `job_id` unless `detach_on_timeout` is `false`.
- `jjmcp_eval_async`: submit Julia code and return a `job_id` at once. The tmux marker path only.
- `jjmcp_wait`: wait for a job or marker id to finish, without resending the code.
- `jjmcp_job_status`: state, elapsed time, last-output age, and the REPL pid, cpu seconds and RSS
  read from `/proc`.
- `jjmcp_result`: fetch a job result from memory or from the on-disk result store.
- `jjmcp_capture_job`: return the output of one job by marker instead of the last N pane lines.
- `jjmcp_list_jobs`: list the jobs this jjmcp process tracks.
- `jjmcp_capture`: capture recent output without sending input.
- `jjmcp_interrupt`: send Ctrl-C to the bound pane.
- `jjmcp_revise`: run `using Revise; Revise.revise()` through `@JJMCP_COMMAND`.
- `jjmcp_activate`: run `using Pkg; Pkg.activate(path)` through `@JJMCP_COMMAND`.
- `jjmcp_test`: run `test_expr`, include a test file, or run `Pkg.test()` through `@JJMCP_COMMAND`.
- `jjmcp_pkg_status`: run `using Pkg; Pkg.status()` through `@JJMCP_COMMAND`.
- `jjmcp_capture_test_results`: parse the latest Julia `Test Summary:` block from tmux output and return
  structured fields like `found_summary`, `test_pass`, `test_fail`, `test_total`, `status`, and `failures`.
- `require_summary` defaults to `false`; when set to `true`, missing summaries return a tool error. `include_raw`
  defaults to `true` and includes captured pane output in `raw_output`.

## Long Runs

Eval-style tools accept `timeout_ms`; the default per-process ceiling is 600000 ms. To permit longer
runs, start the server with a larger ceiling, for example:

```sh
JJMCP_TIMEOUT_MS_MAX=14400000 build/jjmcp serve
```

That example allows calls up to four hours when the client passes a matching `timeout_ms`.

Raising the ceiling is rarely the right answer now. A run that outlives its waiting window is handed
to the background poller instead of failing, so the better pattern for long work is a short
`timeout_ms` plus `jjmcp_wait`. A detached job blocks no MCP call and is bounded separately by
`JJMCP_JOB_MS_MAX` (default 86400000 ms).

Oversized eval output is returned from the end of the captured output, with a truncation marker for
omitted earlier lines or bytes. The full output remains visible in the bound tmux pane scrollback.
Use `jjmcp_capture_job` rather than `jjmcp_capture` for a long run: it returns that job's own output
by marker, so unrelated pane traffic cannot appear in the result.

## Notes

The server sends code by loading a tmux buffer and pasting it into the bound pane. Evaluation output
is isolated by marker lines emitted inside `@JJMCP_COMMAND` and returned as MCP text content. Julia
exceptions are captured as tool errors with the Julia error text.

## License

JJMCP is released under the MIT License. Copyright (c) 2026 John Tinnerholm. See [LICENSE](LICENSE) for the full text.
