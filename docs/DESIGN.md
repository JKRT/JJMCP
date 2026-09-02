# Design Note

JJMCP follows a functional core, imperative shell shape.

Pure or mostly pure code:

- `julia_wrap`: marker generation inputs, Julia wrapper construction, string literal escaping, marker extraction, and output truncation.
- `tmux` command construction through argv vectors.
- Tool schema construction.

Imperative shell:

- `main`: stdio MCP loop and process lifetime.
- `mcp`: JSON-RPC framing and dispatch.
- `process`: POSIX `fork`, `execvp`, pipes, `poll`, timeouts, and child collection.
- `tmux`: the only boundary that invokes `tmux`.
- `state`: in-memory binding plus `.jjmcp/config.json` persistence.
- `tools`: validates MCP tool arguments, coordinates tmux calls, and updates server state.

The server intentionally sends work into the user's existing tmux Julia REPL instead of creating hidden Julia workers. This keeps package state, Revise state, loaded modules, active project, and user-visible output aligned with the workflow the user already controls.

For tmux eval transport, JJMCP installs a small `Main.JJMCPRuntime` helper module in the bound REPL
and then pastes eval-style work as `@JJMCP_COMMAND "<marker-id>" <timeout_ms> begin ... end`.
`jjmcp_eval`, `jjmcp_revise`, `jjmcp_activate`, `jjmcp_test`, and `jjmcp_pkg_status` all share this
path. The macro emits the private markers used for MCP extraction and evaluates user statements in
the caller module so globals and imports persist in the same REPL session. Non-eval tools only call
tmux or inspect state and do not paste Julia code.

## Jobs and the control plane

A tmux evaluation is tracked as a job whose id is its marker id. `jobs` holds that layer:

- `MarkerPoller` owns the capture accumulator for one marker and is resumable. A foreground wait can
  stop polling and hand the same accumulator to a background thread, so nothing is lost at the
  handoff. This is what makes a detached run recoverable rather than merely re-observable.
- `EvalJob` separates immutable identity from a mutex-guarded live view that the polling thread
  republishes. `JobStore` bounds how many jobs stay in memory, keeps at most one job in flight per
  pane, owns the poller threads, and persists finished results under `.jjmcp/jobs`.
- The advisory pane lock is owned by the job, not by the tool call, so a detached job still reserves
  the pane.

Threading follows from that split. `tools/call` still runs on the single worker thread, because the
dispatcher, the binding, and the marker sequence are not synchronized. The job tools that read only
the job store (`jjmcp_job_status`, `jjmcp_result`, `jjmcp_capture_job`, `jjmcp_list_jobs`) are
answered on the read thread instead, which is what lets them report process activity while the
worker is inside a long evaluation. Process counters come from `/proc`, never from the REPL, so they
stay available while Julia is busy.

Because poller threads and the worker thread both fork `tmux`, the child side of `process` does no
work that is not async-signal-safe: argv is materialized before the fork.

Errors from malformed JSON-RPC requests are protocol errors. Errors from tmux or Julia execution are returned as MCP tool results with `isError: true`, so the server remains alive and the caller can inspect the text.
