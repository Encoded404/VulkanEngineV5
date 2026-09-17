module;

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

module Examples.InfiniteRunner.Leaderboard.Transport;

import std;

import Examples.InfiniteRunner.Leaderboard.Log;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

[[nodiscard]] NativeSocket ToNative(TcpSocket::NativeHandle handle) noexcept {
    return static_cast<NativeSocket>(handle);
}

[[nodiscard]] TcpSocket::NativeHandle FromNative(NativeSocket socket) noexcept {
    return static_cast<TcpSocket::NativeHandle>(socket);
}

void CloseNative(NativeSocket socket) noexcept {
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

void EnsureWinsock() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        return true;
    }();
    (void)initialized;
#endif
}

[[nodiscard]] std::optional<timeval> ToTimeval(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::nullopt;
    }
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
    return tv;
}

void SetNonBlocking(NativeSocket socket, bool enable) noexcept {
#ifdef _WIN32
    u_long mode = enable ? 1UL : 0UL;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return;
    }
    fcntl(socket, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

void SetSocketTimeout(NativeSocket socket, int option, std::chrono::milliseconds timeout) noexcept {
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(timeout.count());
    setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    const std::optional<timeval> tv = ToTimeval(timeout);
    if (tv.has_value()) {
        setsockopt(socket, SOL_SOCKET, option, &*tv, sizeof(*tv));
    }
#endif
}

// A signal delivered while a blocking socket call is in progress makes it
// return early with EINTR/WSAEINTR. The call did not fail; it was interrupted.
// A GUI process (SDL installs signal handlers) sees this often enough that
// treating it as an error makes connections look randomly flaky.
[[nodiscard]] bool IsInterrupted() noexcept {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

// select() said readable but accept() found nothing: a spurious wakeup, not an
// error. Only meaningful for a non-blocking listener.
[[nodiscard]] bool WouldBlock() noexcept {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// SO_RCVTIMEO/SO_SNDTIMEO expiry. POSIX reports EAGAIN/EWOULDBLOCK; Winsock
// reports WSAETIMEDOUT for a timeout, so WouldBlock alone is not enough.
[[nodiscard]] bool IsTimeout() noexcept {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// The peer is gone: clean shutdown (recv == 0) is handled by the caller, this
// covers a reset or a write to a connection the peer already closed.
[[nodiscard]] bool IsDisconnected() noexcept {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN ||
           error == WSAESHUTDOWN || error == WSAETIMEDOUT;
#else
    return errno == ECONNRESET || errno == ECONNABORTED || errno == ENOTCONN || errno == EPIPE ||
           errno == ETIMEDOUT;
#endif
}

// select() keeps the placeholder portable between POSIX and Winsock. Both wait
// helpers retry on EINTR against a steady deadline, so frequent signals cannot
// extend the wait past the caller's timeout.
[[nodiscard]] bool WaitReadable(NativeSocket socket, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (timeout > std::chrono::milliseconds::zero() && remaining <= std::chrono::milliseconds::zero()) {
            return false;
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);
        std::optional<timeval> tv = ToTimeval(remaining);
        const int rc = ::select(static_cast<int>(socket) + 1, &read_set, nullptr, nullptr,
                                tv.has_value() ? &*tv : nullptr);
        if (rc >= 0) {
            return rc > 0;
        }
        if (!IsInterrupted()) {
            return false;
        }
    }
}

[[nodiscard]] bool WaitWritable(NativeSocket socket, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (timeout > std::chrono::milliseconds::zero() && remaining <= std::chrono::milliseconds::zero()) {
            return false;
        }
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket, &write_set);
        std::optional<timeval> tv = ToTimeval(remaining);
        const int rc = ::select(static_cast<int>(socket) + 1, nullptr, &write_set, nullptr,
                                tv.has_value() ? &*tv : nullptr);
        if (rc >= 0) {
            return rc > 0;
        }
        if (!IsInterrupted()) {
            return false;
        }
    }
}

[[nodiscard]] std::optional<NativeSocket> OpenConnected(std::string_view host, std::uint16_t port,
                                                        std::chrono::milliseconds timeout) {
    EnsureWinsock();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string host_text{host};
    const std::string port_text = std::to_string(port);
    LogMessage(LogLevel::Debug, std::format("transport: connecting to {}:{}", host_text, port_text));

    addrinfo* results = nullptr;
    if (getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &results) != 0) {
        LogMessage(LogLevel::Warn,
                   std::format("transport: cannot resolve {}:{}", host_text, port_text));
        return std::nullopt;
    }

    std::optional<NativeSocket> connected;
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const NativeSocket socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket == kInvalidSocket) {
            continue;
        }

        SetNonBlocking(socket, true);
        int rc = ::connect(socket, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen));
        bool ok = rc == 0;
        if (!ok) {
            ok = WaitWritable(socket, timeout);
            if (ok) {
                int error = 0;
                socklen_t length = sizeof(error);
#ifdef _WIN32
                getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
#else
                getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length);
#endif
                ok = error == 0;
            }
        }

        if (ok) {
            SetNonBlocking(socket, false);
            connected = socket;
            break;
        }
        CloseNative(socket);
    }

    freeaddrinfo(results);
    if (!connected.has_value()) {
        LogMessage(LogLevel::Warn,
                   std::format("transport: connect to {}:{} failed (timeout or refused)",
                               host_text, port_text));
    }
    return connected;
}

} // namespace

TcpSocket::~TcpSocket() {
    Close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

std::optional<TcpSocket> TcpSocket::Connect(std::string_view host, std::uint16_t port,
                                            std::chrono::milliseconds timeout) {
    const std::optional<NativeSocket> socket = OpenConnected(host, port, timeout);
    if (!socket.has_value()) {
        return std::nullopt;
    }
    LogMessage(LogLevel::Debug,
               std::format("transport: connected to {}:{}", host, port));
    TcpSocket result(FromNative(*socket));
    result.SetTimeouts(std::chrono::seconds(5), std::chrono::seconds(5));
    return result;
}

IoStatus TcpSocket::SendAll(std::span<const std::byte> data) {
    if (!IsOpen()) {
        return IoStatus::Disconnected;
    }
    const NativeSocket socket = ToNative(handle_);
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int rc = ::send(socket,
                              reinterpret_cast<const char*>(data.data() + sent),
                              static_cast<int>(data.size() - sent),
                              kSendFlags);
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        // Interrupted by a signal: the send did not fail, so resume it.
        if (rc < 0 && IsInterrupted()) {
            continue;
        }
        if (IsTimeout()) {
            return IoStatus::Timeout;
        }
        if (IsDisconnected()) {
            return IoStatus::Disconnected;
        }
        return IoStatus::Error;
    }
    return IoStatus::Ok;
}

IoStatus TcpSocket::RecvSome(std::span<std::byte> out, std::size_t& received) {
    received = 0;
    if (!IsOpen()) {
        return IoStatus::Disconnected;
    }
    for (;;) {
        const int rc = ::recv(ToNative(handle_), reinterpret_cast<char*>(out.data()),
                              static_cast<int>(out.size()), 0);
        if (rc > 0) {
            received = static_cast<std::size_t>(rc);
            return IoStatus::Ok;
        }
        if (rc == 0) {
            return IoStatus::Disconnected; // clean EOF
        }
        // Interrupted by a signal: the recv did not fail, so retry it.
        if (IsInterrupted()) {
            continue;
        }
        if (IsTimeout()) {
            return IoStatus::Timeout;
        }
        if (IsDisconnected()) {
            return IoStatus::Disconnected;
        }
        return IoStatus::Error;
    }
}

IoStatus TcpSocket::RecvExactly(std::span<std::byte> out) {
    std::size_t got = 0;
    while (got < out.size()) {
        std::size_t chunk = 0;
        const IoStatus status = RecvSome(out.subspan(got), chunk);
        if (status != IoStatus::Ok) {
            // A timeout that lands after some bytes of this frame have already
            // arrived leaves the stream mid-frame, so it can no longer be
            // resumed by the next call: report the connection as unusable.
            if (status == IoStatus::Timeout && got > 0) {
                return IoStatus::Disconnected;
            }
            return status;
        }
        if (chunk == 0) {
            return IoStatus::Disconnected; // EOF before the requested bytes arrived
        }
        got += chunk;
    }
    return IoStatus::Ok;
}

void TcpSocket::SetTimeouts(std::chrono::milliseconds recv_timeout, std::chrono::milliseconds send_timeout) {
    if (!IsOpen()) {
        return;
    }
    SetSocketTimeout(ToNative(handle_), SO_RCVTIMEO, recv_timeout);
    SetSocketTimeout(ToNative(handle_), SO_SNDTIMEO, send_timeout);
}

void TcpSocket::Shutdown() {
    if (handle_ == -1) {
        return;
    }
#ifdef _WIN32
    ::shutdown(ToNative(handle_), SD_BOTH);
#else
    ::shutdown(ToNative(handle_), SHUT_RDWR);
#endif
}

void TcpSocket::Close() {
    if (handle_ != -1) {
        CloseNative(ToNative(handle_));
        handle_ = -1;
    }
}

bool TcpSocket::IsOpen() const {
    return handle_ != -1;
}

TcpListener::~TcpListener() {
    Close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = -1;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = -1;
        other.port_ = 0;
    }
    return *this;
}

std::optional<TcpListener> TcpListener::Bind(std::uint16_t port) {
    EnsureWinsock();

    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        LogMessage(LogLevel::Error, "transport: cannot create a listening socket");
        return std::nullopt;
    }

    int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        LogMessage(LogLevel::Error,
                   std::format("transport: cannot bind port {}{}", port,
                               port == 0 ? "" : " (already in use?)"));
        CloseNative(socket);
        return std::nullopt;
    }
    if (::listen(socket, 8) != 0) {
        LogMessage(LogLevel::Error, std::format("transport: cannot listen on port {}", port));
        CloseNative(socket);
        return std::nullopt;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    std::uint16_t bound_port = port;
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port = ntohs(bound.sin_port);
    }

    // A non-blocking listener means accept() can never block. Readiness and the
    // accept are not atomic, so a connection can disappear in between; with a
    // blocking listener that race hangs the accept loop and every later
    // handshake times out.
    SetNonBlocking(socket, true);

    return TcpListener(FromNative(socket), bound_port);
}

std::optional<TcpSocket> TcpListener::Accept(std::chrono::milliseconds timeout) {
    if (!IsOpen()) {
        return std::nullopt;
    }
    const NativeSocket socket = static_cast<NativeSocket>(handle_);
    if (!WaitReadable(socket, timeout)) {
        return std::nullopt;
    }

    sockaddr_in client{};
    socklen_t client_len = sizeof(client);
    NativeSocket accepted = kInvalidSocket;
    for (;;) {
        client_len = sizeof(client);
        accepted = ::accept(socket, reinterpret_cast<sockaddr*>(&client), &client_len);
        if (accepted != kInvalidSocket) {
            break;
        }
        if (IsInterrupted()) {
            continue; // a signal is not a failure
        }
        if (WouldBlock()) {
            return std::nullopt; // spurious readiness
        }
        return std::nullopt;
    }
    // accept() may inherit the listener's non-blocking flag on some platforms;
    // the connection is used with timeouts and must block.
    SetNonBlocking(accepted, false);

    char address_text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &client.sin_addr, address_text, sizeof(address_text));
    LogMessage(LogLevel::Debug,
               std::format("transport: accepted connection from {}:{}", address_text,
                           ntohs(client.sin_port)));

    TcpSocket result = TcpSocket::Adopt(FromNative(accepted));
    result.SetTimeouts(std::chrono::seconds(10), std::chrono::seconds(10));
    return result;
}

std::uint16_t TcpListener::BoundPort() const {
    return port_;
}

void TcpListener::Close() {
    if (handle_ != -1) {
        CloseNative(static_cast<NativeSocket>(handle_));
        handle_ = -1;
    }
}

bool TcpListener::IsOpen() const {
    return handle_ != -1;
}

IoStatus SendFrame(TcpSocket& socket, std::span<const std::byte> payload) {
    if (payload.size() > kMaxFrameBytes) {
        LogMessage(LogLevel::Warn,
                   std::format("transport: refusing to send {} byte frame (limit {})",
                               payload.size(), kMaxFrameBytes));
        return IoStatus::Error;
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    std::array<std::byte, 4> prefix{};
    prefix[0] = static_cast<std::byte>(length & 0xFFU);
    prefix[1] = static_cast<std::byte>((length >> 8) & 0xFFU);
    prefix[2] = static_cast<std::byte>((length >> 16) & 0xFFU);
    prefix[3] = static_cast<std::byte>((length >> 24) & 0xFFU);
    const IoStatus header_status = socket.SendAll(prefix);
    if (header_status != IoStatus::Ok) {
        return header_status;
    }
    return socket.SendAll(payload);
}

IoStatus RecvFrame(TcpSocket& socket, std::vector<std::byte>& payload) {
    std::array<std::byte, 4> prefix{};
    const IoStatus prefix_status = socket.RecvExactly(prefix);
    if (prefix_status != IoStatus::Ok) {
        return prefix_status;
    }
    const auto length = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[0])) |
                        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[1])) << 8) |
                        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[2])) << 16) |
                        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[3])) << 24);
    if (length > kMaxFrameBytes) {
        LogMessage(LogLevel::Warn,
                   std::format("transport: peer announced a {} byte frame (limit {})", length,
                               kMaxFrameBytes));
        return IoStatus::Error;
    }
    payload.resize(length);
    if (length == 0) {
        return IoStatus::Ok;
    }
    return socket.RecvExactly(payload);
}

} // namespace Examples::InfiniteRunner::Leaderboard
