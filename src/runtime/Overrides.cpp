module;

module Runtime.Overrides;

import std;

import VulkanEngine.GplPolicy;

namespace Runtime {

// ── Choice machinery (non-exported; only the built-in specs and this unit use
//    the templates, so they live here rather than in the interface TU) ──────
std::string JoinStrings(const std::vector<std::string>& values, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += sep;
        out += values[i];
    }
    return out;
}

template <typename T>
inline constexpr std::span<const Choice<T>> kChoicesOf{};

template <>
inline constexpr std::span<const Choice<GplPolicy>> kChoicesOf<GplPolicy> = kGplChoices;

template <>
inline constexpr std::span<const Choice<GplStructurePolicy>> kChoicesOf<GplStructurePolicy> = kGplStructureChoices;

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

// ── Built-in specs ─────────────────────────────────────────────────────────
inline constexpr std::array kBuiltinSpecs{
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

// ── Overrides implementations ─────────────────────────────────────────────
void Overrides::Register(OverrideSpec spec) {
    extra_specs_.push_back(std::move(spec));
}

void Overrides::Apply(std::string_view kv) {
    const auto known_keys = [&] {
        std::string keys;
        const auto append = [&](const OverrideSpec& spec) {
            if (!keys.empty()) keys += ", ";
            keys += spec.key;
        };
        for (const auto& spec : kBuiltinSpecs) append(spec);
        for (const auto& spec : extra_specs_) append(spec);
        return keys;
    }();

    const std::size_t eq = kv.find('=');
    if (eq == std::string_view::npos) {
        throw OverrideError("missing '=' in overwrite '" + std::string{kv} +
                            "' (expected <key>=<value>; known keys: " + known_keys + ")");
    }
    const std::string_view key = kv.substr(0, eq);
    const std::string_view value = kv.substr(eq + 1);
    for (const auto& spec : kBuiltinSpecs) {
        if (spec.key == key) {
            spec.apply(value, *this);
            return;
        }
    }
    for (const auto& spec : extra_specs_) {
        if (spec.key == key) {
            spec.apply(value, *this);
            return;
        }
    }
    throw OverrideError("unknown overwrite key '" + std::string{key} + "' (known keys: " + known_keys + ")");
}

std::string Overrides::GenerateHelp() const {
    // Alignment column for the "<key>=<hint>" part: widest entry plus a small
    // margin, so every line lines up no matter how long the value grammar
    // grows (a fixed width would underflow on long hints).
    std::size_t key_width = 0;
    const auto measure = [&](const OverrideSpec& spec) {
        key_width = std::max(key_width, 2 + spec.key.size() + 1 + spec.value_hint().size());
    };
    for (const auto& spec : kBuiltinSpecs) measure(spec);
    for (const auto& spec : extra_specs_) measure(spec);

    const std::string detail_indent(key_width + 7, ' ');
    std::string out = "Overwrite knobs (--overwrite <key>=<value>):\n\n";
    const auto emit = [&](const OverrideSpec& spec) {
        const std::string key_value = std::string{spec.key} + "=" + spec.value_hint();
        std::string line = "  " + key_value;
        line += std::string(key_width + 5 - line.size(), ' ') + "  " + std::string{spec.summary} + "\n";
        if (!spec.details.empty() || spec.repeatable) {
            std::string details;
            if (!spec.details.empty()) details += spec.details;
            if (spec.repeatable) {
                if (!details.empty()) details += "  ";
                details += "[repeatable]";
            }
            line += detail_indent + details + "\n";
        }
        if (spec.choices_help) {
            const std::string choices = spec.choices_help();
            std::size_t pos = 0;
            while (pos < choices.size()) {
                const std::size_t nl = choices.find('\n', pos);
                const std::string_view piece = (nl == std::string::npos)
                    ? std::string_view{choices}.substr(pos)
                    : std::string_view{choices}.substr(pos, nl - pos);
                line += detail_indent + std::string{piece} + "\n";
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
        line += detail_indent + "default: " + spec.describe(Overrides{}) +
                " | current: " + spec.describe(*this) + "\n";
        out += line;
    };
    for (const auto& spec : kBuiltinSpecs) emit(spec);
    for (const auto& spec : extra_specs_) emit(spec);
    return out;
}

std::string Overrides::DescribeEffective() const {
    std::string out;
    const auto append = [&](const OverrideSpec& spec) {
        if (!out.empty()) out += ", ";
        out += std::string{spec.key} + "=" + spec.describe(*this);
    };
    for (const auto& spec : kBuiltinSpecs) append(spec);
    for (const auto& spec : extra_specs_) append(spec);
    return out;
}

} // namespace Runtime