#include "socket_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace jjmcp {

std::string SocketClient::default_socket_path(const std::string& pane_id)
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string runtime_dir = runtime != nullptr && runtime[0] != '\0' ? runtime : "/tmp";
    std::string sanitized = pane_id;
    sanitized.erase(std::remove(sanitized.begin(), sanitized.end(), '%'), sanitized.end());
    if (sanitized.empty()) {
        sanitized = "default";
    }
    return runtime_dir + "/jjmcp-" + sanitized + ".sock";
}

bool SocketClient::socket_exists(const std::string& path)
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISSOCK(st.st_mode);
}

Result<SocketEvalResponse> SocketClient::eval(const std::string& path,
                                              const std::string& code,
                                              std::chrono::milliseconds timeout,
                                              std::size_t max_output_bytes) const
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<SocketEvalResponse>::failure(std::string("socket(): ") + std::strerror(errno));
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return Result<SocketEvalResponse>::failure("socket path exceeds sockaddr_un.sun_path: " + path);
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int err = errno;
        ::close(fd);
        return Result<SocketEvalResponse>::failure(std::string("connect(") + path + "): " + std::strerror(err));
    }

    timeval tv {};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    nlohmann::json request = {
        {"op", "eval"},
        {"code", code},
        {"max_output_bytes", max_output_bytes},
    };
    std::string payload = request.dump();
    payload.push_back('\n');

    std::size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t n = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
        if (n <= 0) {
            const int err = errno;
            ::close(fd);
            return Result<SocketEvalResponse>::failure(std::string("send(): ") + std::strerror(err));
        }
        sent += static_cast<std::size_t>(n);
    }

    std::string buf;
    char chunk[4096];
    while (buf.find('\n') == std::string::npos) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
            ::close(fd);
            return Result<SocketEvalResponse>::failure("server closed the connection before responding");
        }
        if (n < 0) {
            const int err = errno;
            ::close(fd);
            if (err == EAGAIN || err == EWOULDBLOCK) {
                return Result<SocketEvalResponse>::failure("socket read timed out");
            }
            return Result<SocketEvalResponse>::failure(std::string("recv(): ") + std::strerror(err));
        }
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fd);

    const auto newline = buf.find('\n');
    const std::string line = buf.substr(0, newline);

    SocketEvalResponse response;
    try {
        const auto doc = nlohmann::json::parse(line);
        response.ok = doc.value("ok", false);
        response.stdout_text = doc.value("stdout", std::string {});
        response.stderr_text = doc.value("stderr", std::string {});
        response.value_show = doc.value("value_show", std::string {});
        response.error_message = doc.value("error_message", std::string {});
        response.backtrace = doc.value("backtrace", std::string {});
        response.elapsed_ms = doc.value("elapsed_ms", 0LL);
    } catch (const std::exception& e) {
        return Result<SocketEvalResponse>::failure(std::string("could not parse response JSON: ") + e.what());
    }
    return Result<SocketEvalResponse>::success(response);
}

} // namespace jjmcp
