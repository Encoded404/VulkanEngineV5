module;

#include <glm/glm.hpp>

#include <imgui.h>

module Examples.Explorer.Options;

import std;

namespace Examples::Explorer {

namespace {

void ShowTooltip(std::string_view tooltip) {
    if (tooltip.empty()) return;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%.*s", static_cast<int>(tooltip.size()), tooltip.data());
    }
}

} // namespace

std::string OptionsUi::MakeLabel(std::string_view label) {
    std::string id{label};
    // ImGui identifies widgets by their label unless "##id" is appended, and
    // the same caption ("Speed") is common across sections.
    id += "##";
    id += std::to_string(next_id_++);
    return id;
}

void OptionsUi::Section(std::string_view label) {
    std::string text{label};
    entries_.push_back([text = std::move(text)] {
        ImGui::SeparatorText(text.c_str());
    });
}

void OptionsUi::Separator() {
    entries_.push_back([] { ImGui::Separator(); });
}

void OptionsUi::Text(std::string_view text) {
    std::string owned{text};
    entries_.push_back([owned = std::move(owned)] {
        ImGui::TextUnformatted(owned.c_str());
    });
}

void OptionsUi::TextDisabled(std::string_view text) {
    std::string owned{text};
    entries_.push_back([owned = std::move(owned)] {
        ImGui::TextDisabled("%s", owned.c_str());
    });
}

void OptionsUi::TextWrapped(std::string_view text) {
    std::string owned{text};
    entries_.push_back([owned = std::move(owned)] {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(owned.c_str());
        ImGui::PopTextWrapPos();
    });
}

void OptionsUi::TextColored(const glm::vec3& color, std::string_view text) {
    std::string owned{text};
    entries_.push_back([color, owned = std::move(owned)] {
        ImGui::TextColored(ImVec4(color.r, color.g, color.b, 1.0f), "%s", owned.c_str());
    });
}

void OptionsUi::ProgressBar(float fraction, std::string_view overlay) {
    std::string owned{overlay};
    entries_.push_back([fraction, owned = std::move(owned)] {
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f),
                           owned.empty() ? nullptr : owned.c_str());
    });
}

void OptionsUi::SameLine() {
    // SameLine applies to the previous widget, so it is an entry like any other.
    entries_.push_back([] { ImGui::SameLine(); });
}

void OptionsUi::Checkbox(std::string_view label, bool& value, std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string tip{tooltip};
    entries_.push_back([this, id = std::move(id), &value, tip = std::move(tip)] {
        if (ImGui::Checkbox(id.c_str(), &value)) changed_ = true;
        ShowTooltip(tip);
    });
}

void OptionsUi::SliderFloat(std::string_view label, float& value, float min, float max,
                            std::string_view format, std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string fmt{format};
    std::string tip{tooltip};
    entries_.push_back(
        [this, id = std::move(id), &value, min, max, fmt = std::move(fmt), tip = std::move(tip)] {
            if (ImGui::SliderFloat(id.c_str(), &value, min, max, fmt.c_str())) changed_ = true;
            ShowTooltip(tip);
        });
}

void OptionsUi::SliderInt(std::string_view label, int& value, int min, int max,
                          std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string tip{tooltip};
    entries_.push_back([this, id = std::move(id), &value, min, max, tip = std::move(tip)] {
        if (ImGui::SliderInt(id.c_str(), &value, min, max)) changed_ = true;
        ShowTooltip(tip);
    });
}

void OptionsUi::DragFloat(std::string_view label, float& value, float speed, float min, float max,
                          std::string_view format, std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string fmt{format};
    std::string tip{tooltip};
    entries_.push_back(
        [this, id = std::move(id), &value, speed, min, max, fmt = std::move(fmt), tip = std::move(tip)] {
            if (ImGui::DragFloat(id.c_str(), &value, speed, min, max, fmt.c_str())) changed_ = true;
            ShowTooltip(tip);
        });
}

void OptionsUi::DragFloat3(std::string_view label, glm::vec3& value, float speed,
                           std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string tip{tooltip};
    entries_.push_back([this, id = std::move(id), &value, speed, tip = std::move(tip)] {
        if (ImGui::DragFloat3(id.c_str(), &value.x, speed)) changed_ = true;
        ShowTooltip(tip);
    });
}

void OptionsUi::Color3(std::string_view label, glm::vec3& value, std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string tip{tooltip};
    entries_.push_back([this, id = std::move(id), &value, tip = std::move(tip)] {
        if (ImGui::ColorEdit3(id.c_str(), &value.x)) changed_ = true;
        ShowTooltip(tip);
    });
}

void OptionsUi::Combo(std::string_view label, int& index, std::span<const std::string_view> items,
                      std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string tip{tooltip};
    std::vector<std::string> owned;
    owned.reserve(items.size());
    for (const std::string_view item : items) owned.emplace_back(item);
    entries_.push_back([this, id = std::move(id), &index, owned = std::move(owned),
                        tip = std::move(tip)] {
        std::vector<const char*> pointers;
        pointers.reserve(owned.size());
        for (const std::string& item : owned) pointers.push_back(item.c_str());
        const int count = static_cast<int>(pointers.size());
        if (ImGui::Combo(id.c_str(), &index, pointers.data(), count)) changed_ = true;
        ShowTooltip(tip);
    });
}

void OptionsUi::Button(std::string_view label, Action action, std::string_view tooltip) {
    std::string id = MakeLabel(label);
    std::string tip{tooltip};
    entries_.push_back([id = std::move(id), action = std::move(action), tip = std::move(tip)] {
        if (ImGui::Button(id.c_str())) action();
        ShowTooltip(tip);
    });
}

void OptionsUi::Custom(std::function<void()> draw) {
    entries_.push_back([draw = std::move(draw)] { draw(); });
}

void OptionsUi::Dynamic(std::function<void(OptionsUi&)> build) {
    entries_.push_back([build = std::move(build)] {
        OptionsUi nested{};
        build(nested);
        nested.Draw();
    });
}

void OptionsUi::Clear() {
    entries_.clear();
    next_id_ = 0;
    changed_ = false;
}

bool OptionsUi::Draw() {
    changed_ = false;
    for (const auto& entry : entries_) entry();
    return changed_;
}

} // namespace Examples::Explorer
