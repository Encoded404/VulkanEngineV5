module;

export module VulkanEngine.PhysicalCameraTypes;

import std;

export namespace VulkanEngine::PhysicalCamera {

// Source pixel formats handled by the PhysicalCamera system. Frames are kept
// in the camera's native format on the GPU; conversion to RGB happens in the
// composite shader (never on the CPU).
enum class PhysicalCameraPixelFormat : std::uint8_t {
    Unknown = 0,
    Xrgb8888, // 4-byte BGRX (SDL byte order), trivial swizzle in shader
    Yuy2,     // packed 4:2:2 (Y0 U Y1 V), YUV->RGB in shader
    Nv12,     // 2-plane 4:2:0 (Y + interleaved UV), YUV->RGB in shader
};

enum class PhysicalCameraPosition : std::uint8_t {
    Unknown,
    FrontFacing,
    BackFacing,
};

enum class PhysicalCameraState : std::uint8_t {
    Closed,
    Opening,
    AwaitingApproval,
    Active,
    Lost,
    Error,
};

enum class FitMode : std::uint8_t {
    Stretch, // ignore aspect ratio
    Contain, // letterbox/pillarbox within the destination rect
    Crop,    // fill the destination rect, cropping overflow
};

struct PhysicalCameraDeviceInfo {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index = 0;
    std::string name{};
    PhysicalCameraPosition position = PhysicalCameraPosition::Unknown;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct PhysicalCameraFormatInfo {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    PhysicalCameraPixelFormat format = PhysicalCameraPixelFormat::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_numerator = 0;
    std::uint32_t fps_denominator = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct PhysicalCameraOpenConfig {
    // Preferred source formats in priority order; the first format the device
    // supports (natively or via SDL conversion, e.g. MJPEG decode) is chosen.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<PhysicalCameraPixelFormat> preferred_formats{
        PhysicalCameraPixelFormat::Nv12,
        PhysicalCameraPixelFormat::Yuy2,
        PhysicalCameraPixelFormat::Xrgb8888,
    };
    std::uint32_t width = 0;  // 0 = device default
    std::uint32_t height = 0;
    std::uint32_t fps = 0;    // 0 = device default
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// A binding draws one camera stream into a target texture at a configured
// destination rect, cropping from the source. Multiple bindings may share a
// target (multi-camera grid); one stream may bind to many targets.
struct PhysicalCameraBindingConfig {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // Source crop in camera pixels. A zero extent means "full frame".
    std::int32_t src_x = 0;
    std::int32_t src_y = 0;
    std::uint32_t src_width = 0;
    std::uint32_t src_height = 0;

    // Destination placement in target pixels (Vulkan viewport/scissor style).
    // A zero dst_width/height defaults to the full target.
    float dst_x = 0.0f;
    float dst_y = 0.0f;
    float dst_width = 0.0f;
    float dst_height = 0.0f;

    FitMode fit = FitMode::Stretch;
    bool flip_x = false;
    bool flip_y = false;
    bool clear_before = false; // clear the target first (multi-cam grid cells)
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

struct PhysicalCameraHandle {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
    friend bool operator==(const PhysicalCameraHandle&, const PhysicalCameraHandle&) = default;
};

struct PhysicalCameraBindingHandle {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
    friend bool operator==(const PhysicalCameraBindingHandle&, const PhysicalCameraBindingHandle&) = default;
};

// Opaque id for a composite target texture created by the system.
struct PhysicalCameraTargetId {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }
    friend bool operator==(const PhysicalCameraTargetId&, const PhysicalCameraTargetId&) = default;
};

} // namespace VulkanEngine::PhysicalCamera
