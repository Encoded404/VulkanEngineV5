module;

#include <glm/glm.hpp>

// Declarative option panels for the shared scene explorer.
//
// A scene (or the camera) declares its controls once, with plain C++ calls, and
// the explorer draws them every frame. Every control binds a reference the
// caller owns, so there is no per-frame allocation and no reflection: the same
// "function calls describe the UI" approach ImGui itself uses, wrapped so a
// scene never has to include <imgui.h> or invent unique widget ids.
export module Examples.Explorer.Options;

import std;

export namespace Examples::Explorer {

class OptionsUi {
public:
    using Action = std::function<void()>;

    // ── Layout ──────────────────────────────────────────────────────────
    void Section(std::string_view label);
    void Separator();
    void Text(std::string_view text);
    void TextDisabled(std::string_view text);
    void TextWrapped(std::string_view text);
    void TextColored(const glm::vec3& color, std::string_view text);
    void ProgressBar(float fraction, std::string_view overlay = {});
    void SameLine();

    // ── Stateful controls (any change is reported by Draw) ──────────────
    void Checkbox(std::string_view label, bool& value, std::string_view tooltip = {});
    void SliderFloat(std::string_view label, float& value, float min, float max,
                     std::string_view format = "%.3f", std::string_view tooltip = {});
    void SliderInt(std::string_view label, int& value, int min, int max,
                   std::string_view tooltip = {});
    void DragFloat(std::string_view label, float& value, float speed, float min, float max,
                   std::string_view format = "%.3f", std::string_view tooltip = {});
    void DragFloat3(std::string_view label, glm::vec3& value, float speed,
                    std::string_view tooltip = {});
    void Color3(std::string_view label, glm::vec3& value, std::string_view tooltip = {});
    void Combo(std::string_view label, int& index, std::span<const std::string_view> items,
               std::string_view tooltip = {});

    // ── One-shot actions ────────────────────────────────────────────────
    void Button(std::string_view label, Action action, std::string_view tooltip = {});

    // A clickable row that draws as selected when `selected` is true. The
    // selection itself stays with the caller; `on_click` only fires on a click.
    void Selectable(std::string_view label, bool selected, Action on_click,
                    std::string_view tooltip = {});

    // A single-line text field bound to `value`, capped at `max_length` bytes.
    // The buffer is rebuilt from `value` every draw, so clearing or replacing
    // the string elsewhere shows up immediately; cursor and selection are ImGui
    // state keyed by the widget id, which is why the field must keep a stable
    // position in the declaration order.
    void InputText(std::string_view label, std::string& value, std::size_t max_length = 128,
                   std::string_view tooltip = {});

    // Escape hatch: emit an arbitrary ImGui widget when the controls above are
    // not enough. `draw` must be self-contained (no dangling captures).
    void Custom(std::function<void()> draw);

    // Rebuilds a sub-panel from function calls every frame. Use it for content
    // whose controls depend on live state (an installed/downloading asset, a
    // selection that changes the available buttons); the callback receives a
    // fresh OptionsUi and may declare anything the class offers.
    void Dynamic(std::function<void(OptionsUi&)> build);

    // ── Panel lifetime ──────────────────────────────────────────────────
    void Clear();
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }

    // Runs every entry in declaration order. Returns true when a stateful
    // control changed value during this draw.
    bool Draw();
    [[nodiscard]] bool ChangedLastDraw() const noexcept { return changed_; }

private:
    [[nodiscard]] std::string MakeLabel(std::string_view label);

    std::vector<std::function<void()>> entries_{};
    std::size_t next_id_ = 0;
    bool changed_ = false;
};

} // namespace Examples::Explorer
