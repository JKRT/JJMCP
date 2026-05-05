# Security

JJMCP is a local trusted tool. It can execute arbitrary Julia code in the tmux pane you bind it to.

Only expose this MCP server to trusted local MCP clients. Do not run it as a network service, do not bind it to an untrusted tmux pane, and do not give untrusted agents access to it.

Important properties:

- `jjmcp_eval`, `jjmcp_test`, `jjmcp_activate`, and `jjmcp_revise` execute code in the user's Julia process.
- The bound tmux pane is the source of truth. Anything typed by a user or another tool in that pane may affect subsequent evaluations.
- JJMCP does not sandbox Julia code.
- JJMCP does not intentionally run destructive shell commands, but Julia code can read, write, or delete files with the user's permissions.
- MCP runs over stdio. stdout is reserved for protocol messages in `serve` mode.

Use this server only on machines and repositories where local code execution by the connected agent is acceptable.
