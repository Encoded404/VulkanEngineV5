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

    [[nodiscard]] bool SendAll(std::span<const std::byte> data);
    [[nodiscard]] bool RecvExactly(std::span<std::byte> out);
    // Returns false on error/timeout; `received` is 0 on a clean EOF.
    [[nodiscard]] bool RecvSome(std::span<std::byte> out, std::size_t& received);

    void SetTimeouts(std::chrono::milliseconds recv_timeout, std::chrono::milliseconds send_timeout);
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

// Length-prefixed frame I/O. Both fail rather than throwing.
[[nodiscard]] bool SendFrame(TcpSocket& socket, std::span<const std::byte> payload);
[[nodiscard]] bool RecvFrame(TcpSocket& socket, std::vector<std::byte>& payload);

} // namespace Examples::InfiniteRunner::Leaderboard
