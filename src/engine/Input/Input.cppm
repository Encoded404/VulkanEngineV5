module;

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module VulkanEngine.Input;

import VulkanBackend.Event;

export namespace VulkanEngine::Input {

using ActionId = std::string;

struct ActionState {
    bool active = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool started = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool canceled = false; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct RawInputState {
    std::unordered_set<int32_t> held_keys{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<int32_t> pressed_keys{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<int32_t> released_keys{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<int32_t> held_mouse_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<int32_t> pressed_mouse_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<int32_t> released_mouse_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_delta_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_delta_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float wheel_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float wheel_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
};

enum class BindingType : uint8_t {
    Key,
    MouseButton
};

struct InputBinding {
    BindingType type = BindingType::Key; // NOLINT(misc-non-private-member-variables-in-classes)
    int32_t code = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] static InputBinding Key(int32_t keycode);
    [[nodiscard]] static InputBinding MouseButton(int32_t button);
};

class InputSystem {
public:
    void BeginFrame();
    void ProcessEvent(const VulkanEngine::Backend::Event::IEvent& event);
    void ProcessEvents(const VulkanEngine::Backend::Event::EventList& events);
    void Update();

    void BindAction(ActionId action, InputBinding binding);
    void ClearBindings();

    [[nodiscard]] const RawInputState& GetRawState() const noexcept;
    [[nodiscard]] const ActionState& GetActionState(std::string_view action) const;
    [[nodiscard]] bool IsActionActive(std::string_view action) const;
    [[nodiscard]] bool WasActionStarted(std::string_view action) const;
    [[nodiscard]] bool WasActionCanceled(std::string_view action) const;

private:
    RawInputState raw_state_{};
    std::unordered_map<ActionId, std::vector<InputBinding>> bindings_{};
    std::unordered_map<ActionId, ActionState> action_states_{};
};

}  // namespace VulkanEngine::Input

