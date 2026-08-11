module;

export module App.Overrides;

import std;

import VulkanEngine.GplPolicy;

export namespace App::Overrides {

using VulkanEngine::ShaderSystem::GplPolicy;
using VulkanEngine::ShaderSystem::GplStructurePolicy;

// One possible value for a typed override. Parse, hint, help and format all
// derive from this table — the value grammar lives exactly once.
template <typename T>
struct Choice {
    std::string_view label;        // CLI spelling, e.g. "force"
    T value;                       // typed value it maps to
    std::string_view description;  // one-liner shown in --help
};

// Single source of truth for DEFAULTS: typed initializers. Help displays
// defaults by formatting Overrides{} through each spec's describe().
struct Overrides {
    std::vector<std::string> force_disabled_extensions;
    GplPolicy gpl_policy = GplPolicy::Auto;
    GplStructurePolicy gpl_structure = GplStructurePolicy::Auto;
};

class OverrideError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct OverrideSpec {
    std::string_view key;       // "ext.disable" | "gpl"
    std::string_view summary;   // one-line help
    std::string_view details;   // optional extended text (may be empty)
    bool repeatable = false;    // shown in help as [repeatable]
    std::string (*value_hint)();               // "<extension name>" | generated "auto|force|off"
    void (*apply)(std::string_view value, Overrides&);  // validate + dispatch; throws OverrideError
    std::string (*describe)(const Overrides&);          // format current value for help
    std::string (*choices_help)() = nullptr;            // optional: per-choice lines for --help
};

inline constexpr std::array kGplChoices = std::to_array<Choice<GplPolicy>>({
    {"auto",  GplPolicy::Auto,    "engine policy (RADV workaround applies)"},
    {"force", GplPolicy::Forced,  "bypass GPL policy restrictions, e.g. combined structure on affected RADV; for testing"},
    {"off",   GplPolicy::Disable, "always use monolithic pipelines"},
});

inline constexpr std::array kGplStructureChoices = std::to_array<Choice<GplStructurePolicy>>({
    {"auto",     GplStructurePolicy::Auto,     "engine policy (RADV < Mesa 26 → split, otherwise combined)"},
    {"split",    GplStructurePolicy::Split,    "separate FS/FOI libraries; required on RADV < Mesa 26 (doc §5)"},
    {"combined", GplStructurePolicy::Combined, "single combined FS+FOI library; broken on RADV < Mesa 26 (doc §3.1/§4)"},
});

// ── Reflection seam (P2996-ready) ──────────────────────────────────────────
// The single generator for static-choice tables. Parse, hint, format and help
// for a choice-based key all derive from kChoicesOf<T>.
// TODAY: one hand-written 3-line table per enum (specialization below).
// LATER (Clang 22+, std::meta::enumerators_of): implement the primary template
// generically — labels from name_of, values from value_of, descriptions from a
// per-type lookup — and delete the specializations. Consumers stay unchanged.
template <typename T>
inline constexpr std::span<const Choice<T>> kChoicesOf{};

template <>
inline constexpr std::span<const Choice<GplPolicy>> kChoicesOf<GplPolicy> = kGplChoices;

template <>
inline constexpr std::span<const Choice<GplStructurePolicy>> kChoicesOf<GplStructurePolicy> = kGplStructureChoices;

// ── Generic machinery over kChoicesOf<T> (non-exported; templates MUST be
//    defined here in the interface TU because kOverrideSpecs lambdas call
//    them) ──────────────────────────────────────────────────────────────────
// JoinStrings:       "a, b" over a vector of values
// JoinLabels:        "a|b|c"
// ApplyChoice:       walk kChoicesOf<T>, set out, else throw OverrideError
//                     naming key, allowed labels, offending value
// DescribeChoice:    label of current value ("<unknown>")
// FormatChoicesHelp: one aligned "label  description" line per choice
//                     (label column padded to max label width)

std::string JoinStrings(const std::vector<std::string>& values, std::string_view sep);

template <typename T>
std::string JoinLabels(std::span<const Choice<T>> choices) {
    std::string out;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (i > 0) out += '|';
        out += choices[i].label;
    }
    return out;
}

template <typename T>
void ApplyChoice(std::string_view key, std::string_view value, T& out) {
    static_assert(!kChoicesOf<T>.empty(), "no choices table for T (specialize kChoicesOf)");
    for (const auto& choice : kChoicesOf<T>) {
        if (choice.label == value) {
            out = choice.value;
            return;
        }
    }
    throw OverrideError(std::string{key} + ": unknown value '" + std::string{value} +
                        "' (expected one of: " + JoinLabels(kChoicesOf<T>) + ")");
}

template <typename T>
std::string DescribeChoice(T value) {
    static_assert(!kChoicesOf<T>.empty(), "no choices table for T (specialize kChoicesOf)");
    for (const auto& choice : kChoicesOf<T>) {
        if (choice.value == value) return std::string{choice.label};
    }
    return "<unknown>";
}

template <typename T>
std::string FormatChoicesHelp() {
    static_assert(!kChoicesOf<T>.empty(), "no choices table for T (specialize kChoicesOf)");
    std::size_t label_width = 0;
    for (const auto& choice : kChoicesOf<T>) {
        label_width = std::max(label_width, choice.label.size());
    }
    std::string out;
    for (const auto& choice : kChoicesOf<T>) {
        out += std::string{choice.label};
        out += std::string(label_width - choice.label.size() + 1, ' ');
        out += std::string{choice.description};
        out += '\n';
    }
    return out;
}

// NOTE: aggregate-init both specs directly into the array — do NOT build a
// std::span from an array temporary (dangling), and do NOT use separate named
// spec constants unless the test needs them (it can iterate kOverrideSpecs).
inline constexpr std::array kOverrideSpecs{
    OverrideSpec{
        .key = "ext.disable",
        .summary = "Force-disable a Vulkan extension for fallback testing",
        .details = "Removes the extension from instance/device request sets; dependent extensions cascade.",
        .repeatable = true,
        .value_hint = [] { return std::string{"<extension name>"}; },  // MUST return std::string
        .apply = [](std::string_view value, Overrides& o) {
            if (value.empty()) throw OverrideError("ext.disable: expected an extension name");
            o.force_disabled_extensions.emplace_back(value);
        },
        .describe = [](const Overrides& o) {
            if (o.force_disabled_extensions.empty()) return std::string{"none"};
            return JoinStrings(o.force_disabled_extensions, ", ");
        },
        // .choices_help: unset — not a choice key
    },
    OverrideSpec{
        .key = "gpl",
        .summary = "Graphics pipeline library policy",
        .value_hint = [] { return JoinLabels(kChoicesOf<GplPolicy>); },
        .apply = [](std::string_view value, Overrides& o) { ApplyChoice("gpl", value, o.gpl_policy); },
        .describe = [](const Overrides& o) { return DescribeChoice(o.gpl_policy); },
        .choices_help = [] { return FormatChoicesHelp<GplPolicy>(); },
    },
    OverrideSpec{
        .key = "gpl.structure",
        .summary = "Graphics pipeline library structure (combined FS+FOI vs split)",
        .details = "See docs/RADV-GPL-fast-linking-bug-and-workaround.md §5 for the split structure.",
        .value_hint = [] { return JoinLabels(kChoicesOf<GplStructurePolicy>); },
        .apply = [](std::string_view value, Overrides& o) { ApplyChoice("gpl.structure", value, o.gpl_structure); },
        .describe = [](const Overrides& o) { return DescribeChoice(o.gpl_structure); },
        .choices_help = [] { return FormatChoicesHelp<GplStructurePolicy>(); },
    },
};

void ApplyOverride(std::string_view kv, Overrides& out);     // throws OverrideError
std::string GenerateOverridesHelp(const Overrides& current); // walks kOverrideSpecs
std::string DescribeEffective(const Overrides& current);     // "ext.disable=none, gpl=auto"

} // namespace App::Overrides
