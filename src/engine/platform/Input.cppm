module;

export module VulkanEngine.Input;

import std;

import VulkanBackend.Event;
import VulkanBackend.Utils.CallbackList;

constexpr std::uint32_t UINT32_MAX =
    std::numeric_limits<std::uint32_t>::max();

export namespace VulkanEngine::Input {

struct ActionHandle {
    std::uint32_t id = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    
    [[nodiscard]] bool operator==(const ActionHandle& other) const = default;
    [[nodiscard]] bool operator<(const ActionHandle& other) const { return id < other.id; }
};

}  // namespace VulkanEngine::Input

template <>
struct std::hash<VulkanEngine::Input::ActionHandle> {
    std::size_t operator()(const VulkanEngine::Input::ActionHandle& handle) const noexcept {
        return std::hash<std::uint32_t>{}(handle.id);
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
    std::unordered_set<std::int32_t> held_keys{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> pressed_keys{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> released_keys{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> held_mouse_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> pressed_mouse_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> released_mouse_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_delta_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float mouse_delta_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float wheel_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float wheel_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
};

enum class BindingType : std::uint8_t {
    Key,
    MouseButton
};

struct InputBinding {
    BindingType type = BindingType::Key; // NOLINT(misc-non-private-member-variables-in-classes)
    std::int32_t code = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] static InputBinding Key(std::int32_t keycode);
    [[nodiscard]] static InputBinding MouseButton(std::int32_t button);
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

    Utils::ScopedHandle<void()> RegisterActiveCallback(ActionHandle handle, std::function<void()> callback);
    Utils::ScopedHandle<void()> RegisterStartedCallback(ActionHandle handle, std::function<void()> callback);
    Utils::ScopedHandle<void()> RegisterEndedCallback(ActionHandle handle, std::function<void()> callback);

    [[nodiscard]] const std::unordered_map<ActionHandle, std::string>& GetAllActions() const;

private:
    struct ActionCallbacks {
        Utils::CallbackList<void()> on_active{};
        Utils::CallbackList<void()> on_started{};
        Utils::CallbackList<void()> on_ended{};
    };

    RawInputState raw_state_{};
    std::unordered_map<ActionHandle, std::vector<InputBinding>> bindings_{};
    std::unordered_map<ActionHandle, ActionState> action_states_{};
    std::unordered_map<ActionHandle, std::string> action_names_{};
    std::unordered_map<ActionHandle, ActionCallbacks> action_callbacks_{};
    std::uint32_t next_action_id_ = 0;
};

}  // namespace VulkanEngine::Input
