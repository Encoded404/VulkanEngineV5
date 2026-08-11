module;

module App.Overrides;

import std;

namespace App::Overrides {

std::string JoinStrings(const std::vector<std::string>& values, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += sep;
        out += values[i];
    }
    return out;
}

void ApplyOverride(std::string_view kv, Overrides& out) {
    const auto known_keys = [] {
        std::string keys;
        for (const auto& spec : kOverrideSpecs) {
            if (!keys.empty()) keys += ", ";
            keys += spec.key;
        }
        return keys;
    }();

    const std::size_t eq = kv.find('=');
    if (eq == std::string_view::npos) {
        throw OverrideError("missing '=' in overwrite '" + std::string{kv} +
                            "' (expected <key>=<value>; known keys: " + known_keys + ")");
    }
    const std::string_view key = kv.substr(0, eq);
    const std::string_view value = kv.substr(eq + 1);
    for (const auto& spec : kOverrideSpecs) {
        if (spec.key == key) {
            spec.apply(value, out);
            return;
        }
    }
    throw OverrideError("unknown overwrite key '" + std::string{key} + "' (known keys: " + known_keys + ")");
}

std::string GenerateOverridesHelp(const Overrides& current) {
    // Alignment column for the "<key>=<hint>" part: widest entry plus a small
    // margin, so every line lines up no matter how long the value grammar
    // grows (a fixed width would underflow on long hints).
    std::size_t key_width = 0;
    for (const auto& spec : kOverrideSpecs) {
        key_width = std::max(key_width, 2 + spec.key.size() + 1 + spec.value_hint().size());
    }
    const std::string detail_indent(key_width + 7, ' ');
    std::string out = "Overwrite knobs (--overwrite <key>=<value>):\n\n";
    for (const auto& spec : kOverrideSpecs) {
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
                " | current: " + spec.describe(current) + "\n";
        out += line;
    }
    return out;
}

std::string DescribeEffective(const Overrides& current) {
    std::string out;
    for (const auto& spec : kOverrideSpecs) {
        if (!out.empty()) out += ", ";
        out += std::string{spec.key} + "=" + spec.describe(current);
    }
    return out;
}

} // namespace App::Overrides
