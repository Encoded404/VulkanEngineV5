module;

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module VulkanEngine.Input;

import VulkanBackend.Event;

export namespace VulkanEngine::Input {

struct ActionHandle {
    uint32_t id = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    
    [[nodiscard]] bool operator==(const ActionHandle& other) const = default;
    [[nodiscard]] bool operator<(const ActionHandle& other) const { return id < other.id; }
};

}  // namespace VulkanEngine::Input

template <>
struct std::hash<VulkanEngine::Input::ActionHandle> {
    std::size_t operator()(const VulkanEngine::Input::ActionHandle& handle) const noexcept {
        return std::hash<uint32_t>{}(handle.id);
    }
};

export namespace VulkanEngine::Input {

struct ActionState {
    bool active = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool started = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool canceled = false; // NOLINT(misc-non-private-member-variables-in-classes)
    float value = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) for analog input
    float value_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) for 2D analog input
    float value_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) for 2D analog input
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

    [[nodiscard]] ActionHandle BindAction(std::string_view name, InputBinding binding);
    void UnbindAction(ActionHandle handle);
    void ClearBindings();

    [[nodiscard]] const RawInputState& GetRawState() const noexcept;
    [[nodiscard]] const ActionState& GetActionState(ActionHandle handle) const;
    [[nodiscard]] bool IsActionActive(ActionHandle handle) const;
    [[nodiscard]] bool WasActionStarted(ActionHandle handle) const;
    [[nodiscard]] bool WasActionCanceled(ActionHandle handle) const;
    [[nodiscard]] float GetActionValue(ActionHandle handle) const;
    [[nodiscard]] std::pair<float, float> GetActionValue2D(ActionHandle handle) const;

    using ActionCallback = void(*)();
    void SetActionUsedCallback(ActionHandle handle, ActionCallback callback);
    void SetActionUsedStartedCallback(ActionHandle handle, ActionCallback callback);
    void SetActionUsedEndedCallback(ActionHandle handle, ActionCallback callback);

    [[nodiscard]] const std::unordered_map<ActionHandle, std::string>& GetAllActions() const;

private:
    RawInputState raw_state_{};
    std::unordered_map<ActionHandle, std::vector<InputBinding>> bindings_{};
    std::unordered_map<ActionHandle, ActionState> action_states_{};
    std::unordered_map<ActionHandle, std::string> action_names_{};
    std::unordered_map<ActionHandle, ActionCallback> action_used_callbacks_{};
    std::unordered_map<ActionHandle, ActionCallback> action_used_started_callbacks_{};
    std::unordered_map<ActionHandle, ActionCallback> action_used_ended_callbacks_{};
    uint32_t next_action_id_ = 0;
};

}  // namespace VulkanEngine::Input
