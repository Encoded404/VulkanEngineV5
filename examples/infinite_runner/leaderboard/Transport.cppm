module;

export module Examples.InfiniteRunner.Leaderboard.Transport;

import std;

export namespace Examples::InfiniteRunner::Leaderboard {

// ─────────────────────────────────────────────────────────────────────────────
// Placeholder TCP transport.
//
// Deliberately small and dependency-free: a blocking socket wrapper with
// timeouts and length-prefixed framing. It exists so the leaderboard works
// end-to-end today; it is expected to be replaced wholesale by a dedicated
// networking library later. Nothing outside this module should assume BSD
// sockets.
// ─────────────────────────────────────────────────────────────────────────────

// A frame is a u32 little-endian length followed by that many payload bytes.
inline constexpr std::uint32_t kMaxFrameBytes = 64U * 1024U;

// Result of a socket operation. A timeout is deliberately distinct from a
// disconnect: a blocking recv() that hits SO_RCVTIMEO has not failed, and a
// caller that wants an idle deadline (the server) must keep waiting while a
// caller that wants a bounded request (the client) can give up.
enum class IoStatus : std::uint8_t {
    Ok = 0,        // the whole operation completed
    Timeout,       // the configured deadline elapsed; the socket is still usable
    Disconnected,  // clean EOF, or the peer closed/reset the connection
    Error,         // any other socket failure
};

class TcpSocket {
public:
    using NativeHandle = std::intptr_t;

    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    // Resolves `host` and connects, honouring `timeout` for the connect itself.
    [[nodiscard]] static std::optional<TcpSocket> Connect(std::string_view host,
                                                          std::uint16_t port,
                                                          std::chrono::milliseconds timeout);

    // Adopt an already-connected native handle (used by TcpListener::Accept).
    [[nodiscard]] static TcpSocket Adopt(NativeHandle handle) noexcept { return TcpSocket(handle); }

    [[nodiscard]] IoStatus SendAll(std::span<const std::byte> data);
    [[nodiscard]] IoStatus RecvExactly(std::span<std::byte> out);
    // On IoStatus::Ok `received` is the byte count; 0 never means EOF (that is
    // IoStatus::Disconnected). On Timeout nothing was read.
    [[nodiscard]] IoStatus RecvSome(std::span<std::byte> out, std::size_t& received);

    void SetTimeouts(std::chrono::milliseconds recv_timeout, std::chrono::milliseconds send_timeout);
    // Shuts the connection down for both directions. Unlike Close, this is safe
    // to call from another thread to interrupt a blocked recv/send; the owning
    // thread still calls Close.
    void Shutdown();
    void Close();
    [[nodiscard]] bool IsOpen() const;
    [[nodiscard]] NativeHandle Handle() const { return handle_; }

private:
    explicit TcpSocket(NativeHandle handle) : handle_(handle) {}
    NativeHandle handle_ = -1;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    // Binds every interface on `port`; port 0 asks the OS for a free port.
    [[nodiscard]] static std::optional<TcpListener> Bind(std::uint16_t port);
    [[nodiscard]] std::optional<TcpSocket> Accept(std::chrono::milliseconds timeout);
    [[nodiscard]] std::uint16_t BoundPort() const;
    void Close();
    [[nodiscard]] bool IsOpen() const;

private:
    TcpListener(std::intptr_t handle, std::uint16_t port) : handle_(handle), port_(port) {}
    std::intptr_t handle_ = -1;
    std::uint16_t port_ = 0;
};

// Length-prefixed frame I/O. Both never throw; a Timeout leaves the connection
// usable, so callers can retry or enforce their own deadline.
[[nodiscard]] IoStatus SendFrame(TcpSocket& socket, std::span<const std::byte> payload);
[[nodiscard]] IoStatus RecvFrame(TcpSocket& socket, std::vector<std::byte>& payload);

} // namespace Examples::InfiniteRunner::Leaderboard
