module JJMCPHelper

using JSON3
using Sockets

export start, stop, is_running, socket_path

# State held in a Ref so that re-include via Revise does not stomp the running server.
const STATE = Ref{Any}(nothing)

mutable struct ServerState
    server::Any           # Sockets.PipeServer for AF_UNIX listen
    task::Task
    path::String
    stop_requested::Bool
end

"""
    default_socket_path() -> String

Compute the canonical Unix socket path for the running tmux pane:
\$XDG_RUNTIME_DIR/jjmcp-<sanitized_pane>.sock. Falls back to /tmp when
XDG_RUNTIME_DIR is unset and to "default" when TMUX_PANE is unset. Both sides
of jjmcp use the exact same recipe so the C++ client can probe without an
explicit path being passed.
"""
function default_socket_path()
    runtime = get(ENV, "XDG_RUNTIME_DIR", "/tmp")
    pane = get(ENV, "TMUX_PANE", "default")
    sanitized = replace(pane, "%" => "")
    return joinpath(runtime, "jjmcp-$(sanitized).sock")
end

"""
    start(path = default_socket_path()) -> String

Open a Unix socket at `path` and accept newline-delimited JSON requests from
jjmcp. Each eval runs at top level via `Base.include_string(Main, ...)`, so module loads
and binding mutations land in the user's REPL just as if they had typed the
code. Captured stdout / stderr are echoed back to the real terminal so the
human still sees output; the value is also `display`ed. The structured copy
travels back over the socket for the agent.

Returns the bound socket path. Subsequent calls error until `stop()` is invoked.
"""
function start(path::AbstractString = default_socket_path())
    if STATE[] !== nothing
        error("JJMCPHelper is already running on $(STATE[].path); call stop() first")
    end
    ispath(path) && rm(path; force = true)
    server = Sockets.listen(path)
    task = @async _accept_loop(server)
    state = ServerState(server, task, String(path), false)
    STATE[] = state
    @info "JJMCPHelper listening on $(path)"
    return path
end

"""
    stop()

Close the listening socket and drop any pending connections. Safe to call
when not running.
"""
function stop()
    state = STATE[]
    state === nothing && return
    state.stop_requested = true
    try; close(state.server); catch; end
    ispath(state.path) && rm(state.path; force = true)
    STATE[] = nothing
    @info "JJMCPHelper stopped"
    return
end

is_running() = STATE[] !== nothing
socket_path() = STATE[] === nothing ? "" : STATE[].path

function _accept_loop(server)
    while true
        client = try
            accept(server)
        catch e
            STATE[] !== nothing && STATE[].stop_requested && return
            (e isa Base.IOError) && return  # closed socket
            @warn "JJMCPHelper accept error" exception = (e, catch_backtrace())
            return
        end
        @async _handle_client(client)
    end
end

function _handle_client(client)
    try
        while !eof(client)
            line = try
                readline(client)
            catch
                break
            end
            isempty(line) && continue
            @debug "JJMCPHelper got request" line
            response = try
                _handle_request(line)
            catch e
                @error "JJMCPHelper handler crashed" exception = (e, catch_backtrace())
                Dict("ok" => false, "error_kind" => "internal", "error_message" => sprint(showerror, e))
            end
            try
                payload = JSON3.write(response)
                write(client, payload)
                write(client, "\n")
                flush(client)
            catch e
                @error "JJMCPHelper write failed" exception = (e, catch_backtrace())
                break
            end
        end
    finally
        try; close(client); catch; end
    end
end

function _handle_request(line::AbstractString)
    request = try
        JSON3.read(line)
    catch e
        return Dict(
            "ok" => false,
            "error_kind" => "parse",
            "error_message" => sprint(showerror, e),
        )
    end
    op = String(get(request, :op, ""))
    if op == "eval"
        max_output_bytes = Int(get(request, :max_output_bytes, 262144))
        return _run_eval(String(get(request, :code, "")), max_output_bytes)
    elseif op == "ping"
        return Dict("ok" => true, "pong" => true)
    elseif op == "pkg_status"
        return _run_pkg_status()
    else
        return Dict(
            "ok" => false,
            "error_kind" => "unknown_op",
            "error_message" => "unknown op: $op",
        )
    end
end

function _capture_pipe()
    p = Pipe()
    Base.link_pipe!(p; reader_supports_async = true, writer_supports_async = true)
    return p
end

function _unwrap_load_error(e)
    while e isa LoadError
        e = e.error
    end
    return e
end

function _truncate_bytes(text::AbstractString, max_bytes::Integer)
    max_bytes <= 0 && return "[JJMCP truncated: output hidden because max byte count is 0]"
    bytes = ncodeunits(text)
    bytes <= max_bytes && return String(text)

    keep = max_bytes
    while keep > 0 && (codeunit(text, keep + 1) & 0xc0) == 0x80
        keep -= 1
    end
    prefix = keep == 0 ? "" : String(codeunits(text)[1:keep])
    if !isempty(prefix) && !endswith(prefix, "\n")
        prefix *= "\n"
    end
    return prefix * "[JJMCP truncated: omitted $(bytes - keep) byte(s)]"
end

function _run_eval(code::AbstractString, max_output_bytes::Integer = 262144)
    # Julia's redirect_stdout requires a Pipe (or DevNull / Function), not a bare IOBuffer.
    # We pipe stdout/stderr to a background task that reads until the pipe is closed.
    out_pipe = _capture_pipe()
    err_pipe = _capture_pipe()

    out_task = @async read(out_pipe.out, String)
    err_task = @async read(err_pipe.out, String)

    value = nothing
    err_msg = ""
    err_bt = ""
    elapsed_ns::UInt64 = 0
    threw = false

    original_stdout = stdout
    original_stderr = stderr
    redirect_stdout(out_pipe.in)
    redirect_stderr(err_pipe.in)
    t0 = time_ns()
    try
        value = Base.include_string(Main, String(code), "jjmcp_socket_eval")
    catch e
        threw = true
        err_msg = sprint(showerror, _unwrap_load_error(e))
        err_bt = sprint(Base.show_backtrace, catch_backtrace())
    finally
        elapsed_ns = time_ns() - t0
        # Close the write ends so the reader tasks can finish; restore the global streams.
        close(out_pipe.in)
        close(err_pipe.in)
        redirect_stdout(original_stdout)
        redirect_stderr(original_stderr)
    end

    out_text = fetch(out_task)
    err_text = fetch(err_task)

    # Echo captured output back to the real REPL so the human still sees their println output and
    # any warnings, plus the value (or the error). This is what preserves the shared-REPL property.
    isempty(out_text) || print(stdout, out_text)
    isempty(err_text) || print(stderr, err_text)

    value_show = ""
    if !threw && value !== nothing
        try
            value_show = sprint(show, MIME"text/plain"(), value)
            println(stdout, value_show)
        catch
            value_show = sprint(show, value)
        end
    end
    if threw
        # Print the human-visible error to stderr too.
        printstyled(stderr, "ERROR: ", err_msg, '\n'; color = :red)
        isempty(err_bt) || print(stderr, err_bt, '\n')
    end

    return Dict(
        "ok" => !threw,
        "stdout" => _truncate_bytes(out_text, max_output_bytes),
        "stderr" => _truncate_bytes(err_text, max_output_bytes),
        "value_show" => _truncate_bytes(value_show, max_output_bytes),
        "error_message" => _truncate_bytes(err_msg, max_output_bytes),
        "backtrace" => _truncate_bytes(err_bt, max_output_bytes),
        "elapsed_ms" => round(Int, elapsed_ns / 1_000_000),
    )
end

function _run_pkg_status()
    buf = IOBuffer()
    try
        @eval Main using Pkg
        old = stdout
        redirect_stdout(buf)
        try
            Base.invokelatest(Main.Pkg.status)
        finally
            redirect_stdout(old)
        end
        return Dict("ok" => true, "stdout" => String(take!(buf)))
    catch e
        return Dict(
            "ok" => false,
            "error_message" => sprint(showerror, e),
            "backtrace" => sprint(Base.show_backtrace, catch_backtrace()),
        )
    end
end

end # module JJMCPHelper
