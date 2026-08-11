module;

export module VulkanEngine.GplPolicy;

import std;

export namespace VulkanEngine::ShaderSystem {

enum class GplPolicy : std::uint8_t {
    Auto,     // engine policy: RADV workaround applies
    // Bypass GPL policy restrictions, e.g. combined structure on affected RADV;
    // for testing (doc §6).
    Forced,
    Disable,  // always monolithic
};

enum class GplStructurePolicy : std::uint8_t {
    Auto,     // engine decides: RADV < Mesa 26 → Split, otherwise Combined
    // Separate FS and FOI libraries (works everywhere; required on RADV < 26,
    // doc §5).
    Split,
    // Single combined FS+FOI library (broken on RADV < 26, doc §3.1/§4).
    Combined,
};

struct GplResolution {
    bool use_gpl = false;
    GplStructurePolicy structure = GplStructurePolicy::Combined;
    enum class Warning : std::uint8_t {
        None,
        ForcedUnsupported,
        CombinedDenied,
        ForcedCombinedFootgun,
    };
    Warning warning = Warning::None;
    bool affected_radv = false;  // logging context only
};

// VK_DRIVER_ID_MESA_RADV = 3 (Khronos VkDriverId spec). All RADV builds (distro,
// Valve, forks) report this ID; llvmpipe/lavapipe (13), ANV (6), NVK (24), etc.
// differ, so the ID check isolates RADV with no false positives.
inline constexpr std::uint32_t kMesaRadvDriverId = 3;

struct GplDriverVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

// Mesa encodes driverVersion as VK_MAKE_VERSION(major, minor, patch)
// (src/vulkan/util/vk_util.c) — same bit layout as VK_MAKE_API_VERSION(0, m, n,
// p): bits 31..22 major, 21..12 minor, 11..0 patch (doc §11). Devel builds
// encode 99s (devel-99), which this decode preserves — the < 26 gate then
// classifies 26.0.0-devel (reports 25.99.99) as affected, the safe direction.
[[nodiscard]] GplDriverVersion DecodeDriverVersion(std::uint32_t driver_version) {
    return GplDriverVersion{
        .major = (driver_version >> 22) & 0x1FFu,
        .minor = (driver_version >> 12) & 0x3FFu,
        .patch = driver_version & 0xFFFu,
    };
}

// Affected = MESA_RADV with (major, minor) < (26, 0), lexicographic tuple
// compare after decoding (doc §11; user semantics table, §1 of the plan).
// Unknown/zero versions decode to 0.0.0 → affected (conservative). Do not
// compare raw packed values: variant bits and the devel-99 encoding make raw
// compare wrong.
[[nodiscard]] bool IsAffectedRadv(std::uint32_t driver_id, std::uint32_t driver_version) {
    if (driver_id != kMesaRadvDriverId) {
        return false;
    }
    const GplDriverVersion version = DecodeDriverVersion(driver_version);
    return std::tuple{version.major, version.minor} < std::tuple{26u, 0u};
}

// Resolution table (doc §3/§4/§6/§9 + the user semantics table, §1 of the plan):
//
//   eff_structure = Auto ? (affected ? Split : Combined) : explicit
//   !supported        → deny (Forced → ForcedUnsupported)
//   Disable           → deny
//   Forced            → use eff_structure (Combined on affected → footgun)
//   Auto, eff=Split   → use Split
//   Auto, eff=Combined→ affected ? deny (CombinedDenied) : use Combined
[[nodiscard]] GplResolution ResolveGpl(GplPolicy policy,
                                              GplStructurePolicy structure,
                                              bool gpl_supported,
                                              std::uint32_t driver_id,
                                              std::uint32_t driver_version) {
    GplResolution resolution;
    const bool affected = IsAffectedRadv(driver_id, driver_version);
    resolution.affected_radv = affected;
    const GplStructurePolicy effective =
        (structure == GplStructurePolicy::Auto)
            ? (affected ? GplStructurePolicy::Split : GplStructurePolicy::Combined)
            : structure;

    if (!gpl_supported) {
        resolution.use_gpl = false;
        resolution.warning = (policy == GplPolicy::Forced)
                                 ? GplResolution::Warning::ForcedUnsupported
                                 : GplResolution::Warning::None;
        return resolution;
    }
    if (policy == GplPolicy::Disable) {
        resolution.use_gpl = false;
        resolution.warning = GplResolution::Warning::None;
        return resolution;
    }
    if (policy == GplPolicy::Forced) {
        resolution.use_gpl = true;
        resolution.structure = effective;
        resolution.warning =
            (effective == GplStructurePolicy::Combined && affected)
                ? GplResolution::Warning::ForcedCombinedFootgun
                : GplResolution::Warning::None;
        return resolution;
    }
    // GplPolicy::Auto
    if (effective == GplStructurePolicy::Split) {
        resolution.use_gpl = true;
        resolution.structure = GplStructurePolicy::Split;
    } else if (affected) {
        resolution.use_gpl = false;
        resolution.warning = GplResolution::Warning::CombinedDenied;
    } else {
        resolution.use_gpl = true;
        resolution.structure = GplStructurePolicy::Combined;
    }
    return resolution;
}

} // namespace VulkanEngine::ShaderSystem
