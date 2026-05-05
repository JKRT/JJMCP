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

Errors from malformed JSON-RPC requests are protocol errors. Errors from tmux or Julia execution are returned as MCP tool results with `isError: true`, so the server remains alive and the caller can inspect the text.
