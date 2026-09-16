module;

export module VulkanEngine.Input;

import std;

import VulkanBackend.Event;
import VulkanShared.CallbackList;

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

// Included in a binding's `gamepad` field to match any connected gamepad.
constexpr std::uint8_t kAnyGamepad = 0xFF;

// Maximum number of scalar components an action can expose.
constexpr std::size_t kMaxActionComponents = 4;

// Where the raw value for a binding comes from.
enum class BindingSource : std::uint8_t {
    Key,
    MouseButton,
    GamepadButton,
    GamepadAxis,
    MouseAxis,
    MouseWheel
};

// How the summed action vector is post-processed before it is reported.
enum class ActionProcessing : std::uint8_t {
    Direct,        // report the raw sum, no length handling
    ClampLength,   // if |v| > max_length, scale the whole vector down to max_length
    Normalize      // report a unit-length direction (zero stays zero)
};

// Per-action configuration. `dimension` is filled in by the templated
// BindAction<D>() helper; the rest is caller-facing.
struct ActionConfig {
    std::uint8_t dimension = 1; // NOLINT(misc-non-private-member-variables-in-classes)
    ActionProcessing processing = ActionProcessing::Direct; // NOLINT(misc-non-private-member-variables-in-classes)
    float max_length = 1.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float deadzone = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) applied to the summed vector length
    float press_threshold = 0.5f; // NOLINT(misc-non-private-member-variables-in-classes)
    float release_threshold = 0.4f; // NOLINT(misc-non-private-member-variables-in-classes) hysteresis, should be <= press
};

// One binding can drive one or more action components. A 2D stick is two
// bindings (one per axis); this allows a single source to feed several
// components with independent weights when that is wanted.
struct BindingContribution {
    std::uint8_t component = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    float scale = 1.0f; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct InputBinding {
    BindingSource source = BindingSource::Key; // NOLINT(misc-non-private-member-variables-in-classes)
    std::int32_t code = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint8_t gamepad = kAnyGamepad; // NOLINT(misc-non-private-member-variables-in-classes)
    float deadzone = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) applied to the raw source value
    float bias = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) added before scaling (e.g. trigger 0..1 -> -1..1)
    std::array<BindingContribution, kMaxActionComponents> contributions{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint8_t contribution_count = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] static InputBinding Key(std::int32_t keycode);
    [[nodiscard]] static InputBinding MouseButton(std::int32_t button);
    [[nodiscard]] static InputBinding GamepadButton(std::int32_t button, std::uint8_t gamepad = kAnyGamepad);
    [[nodiscard]] static InputBinding GamepadAxis(std::int32_t axis, std::uint8_t gamepad = kAnyGamepad);
    // `axis`: 0 = x, 1 = y.
    [[nodiscard]] static InputBinding MouseAxis(std::int32_t axis);
    [[nodiscard]] static InputBinding MouseWheel(std::int32_t axis);

    // Append a component this binding should drive. With no explicit
    // contribution the binding drives component 0 with scale 1.
    [[nodiscard]] InputBinding Component(std::uint8_t component, float scale = 1.0f) const;
    [[nodiscard]] InputBinding Bias(float bias) const;
    [[nodiscard]] InputBinding Deadzone(float deadzone) const;
    [[nodiscard]] InputBinding OnGamepad(std::uint8_t gamepad) const;
};

struct GamepadRawState {
    std::unordered_set<std::int32_t> held_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> pressed_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_set<std::int32_t> released_buttons{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::unordered_map<std::int32_t, float> axes{}; // NOLINT(misc-non-private-member-variables-in-classes)
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
    // Present entries are connected gamepads, keyed by the stable index the
    // platform backend assigns on connect.
    std::unordered_map<std::uint8_t, GamepadRawState> gamepads{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ActionState {
    bool active = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool started = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool canceled = false; // NOLINT(misc-non-private-member-variables-in-classes)
    float value = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) component 0, for convenience
    float value_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) component 0
    float value_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes) component 1 when present
    std::array<float, kMaxActionComponents> values{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint8_t dimension = 1; // NOLINT(misc-non-private-member-variables-in-classes)
};

class InputSystem;

// Typed view onto an action with D scalar components. Returned by
// InputSystem::BindAction<D>(). Defined after InputSystem below.
template <std::size_t D>
class Action;

class InputSystem {
public:
    void BeginFrame();
    void ProcessEvent(const VulkanBackend::Event::IEvent& event);
    void ProcessEvents(const VulkanBackend::Event::EventList& events);
    void Update();

    [[nodiscard]] ActionHandle BindAction(std::string_view name, InputBinding binding);
    [[nodiscard]] ActionHandle BindAction(std::string_view name, const ActionConfig& config);
    void AddBinding(ActionHandle handle, InputBinding binding);
    void UnbindAction(ActionHandle handle);
    void ClearBindings();

    // Bind an action with D components. The config's `dimension` is overwritten.
    template <std::size_t D>
    [[nodiscard]] Action<D> BindAction(std::string_view name, const ActionConfig& config);

    [[nodiscard]] const RawInputState& GetRawState() const noexcept;
    [[nodiscard]] const ActionState& GetActionState(ActionHandle handle) const;
    [[nodiscard]] bool IsActionActive(ActionHandle handle) const;
    [[nodiscard]] bool WasActionStarted(ActionHandle handle) const;
    [[nodiscard]] bool WasActionCanceled(ActionHandle handle) const;
    [[nodiscard]] float GetActionValue(ActionHandle handle) const;
    [[nodiscard]] std::pair<float, float> GetActionValue2D(ActionHandle handle) const;
    [[nodiscard]] std::span<const float> GetActionVector(ActionHandle handle) const;
    [[nodiscard]] bool IsGamepadConnected(std::uint8_t gamepad) const;

    VulkanShared::ScopedHandle<void()> RegisterActiveCallback(ActionHandle handle, std::function<void()> callback);
    VulkanShared::ScopedHandle<void()> RegisterStartedCallback(ActionHandle handle, std::function<void()> callback);
    VulkanShared::ScopedHandle<void()> RegisterEndedCallback(ActionHandle handle, std::function<void()> callback);

    [[nodiscard]] const std::unordered_map<ActionHandle, std::string>& GetAllActions() const;
    [[nodiscard]] const ActionConfig& GetActionConfig(ActionHandle handle) const;

private:
    struct ActionCallbacks {
        VulkanShared::CallbackList<void()> on_active{};
        VulkanShared::CallbackList<void()> on_started{};
        VulkanShared::CallbackList<void()> on_ended{};
    };

    [[nodiscard]] bool SampleBinding(const InputBinding& binding, float& out) const;

    RawInputState raw_state_{};
    std::unordered_map<ActionHandle, std::vector<InputBinding>> bindings_{};
    std::unordered_map<ActionHandle, ActionConfig> action_configs_{};
    std::unordered_map<ActionHandle, ActionState> action_states_{};
    std::unordered_map<ActionHandle, std::string> action_names_{};
    std::unordered_map<ActionHandle, ActionCallbacks> action_callbacks_{};
    std::uint32_t next_action_id_ = 0;
};

// Typed view onto an action with D scalar components. Cheap to copy; it just
// wraps a handle. Kept after InputSystem so its members can be inline.
template <std::size_t D>
class Action {
public:
    Action() = default;
    Action(InputSystem* system, ActionHandle handle) noexcept : system_(system), handle_(handle) {}

    [[nodiscard]] ActionHandle Handle() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return system_ != nullptr; }

    // Attach a source to this action. Chain with InputBinding helpers, e.g.
    //   move.Bind(InputBinding::Key(SDLK_A).Component(0, -1.0f));
    Action& Bind(InputBinding binding) {
        if (system_ != nullptr) {
            system_->AddBinding(handle_, std::move(binding));
        }
        return *this;
    }

    // Convenience for a standard gamepad stick on components
    // [first_component, first_component + 1]. SDL Y axes point down, hence the
    // default negative y_scale.
    Action& BindGamepadStick(std::int32_t axis_x, std::int32_t axis_y,
                             std::uint8_t first_component = 0,
                             float y_scale = -1.0f,
                             std::uint8_t gamepad = kAnyGamepad)
        requires (D >= 2)
    {
        Bind(InputBinding::GamepadAxis(axis_x, gamepad).Component(first_component, 1.0f));
        Bind(InputBinding::GamepadAxis(axis_y, gamepad).Component(static_cast<std::uint8_t>(first_component + 1), y_scale));
        return *this;
    }

    [[nodiscard]] std::array<float, D> Value() const {
        std::array<float, D> result{};
        if (system_ == nullptr) {
            return result;
        }
        const std::span<const float> values = system_->GetActionVector(handle_);
        for (std::size_t i = 0; i < D && i < values.size(); ++i) {
            result[i] = values[i];
        }
        return result;
    }

    // Component 0 as a plain float (1D actions).
    [[nodiscard]] float Scalar() const
        requires (D == 1)
    {
        return system_ != nullptr ? system_->GetActionValue(handle_) : 0.0f;
    }

    [[nodiscard]] bool Active() const { return system_ != nullptr && system_->IsActionActive(handle_); }
    [[nodiscard]] bool Started() const { return system_ != nullptr && system_->WasActionStarted(handle_); }
    [[nodiscard]] bool Canceled() const { return system_ != nullptr && system_->WasActionCanceled(handle_); }

    VulkanShared::ScopedHandle<void()> OnActive(std::function<void()> callback) {
        if (system_ == nullptr) {
            return {};
        }
        return system_->RegisterActiveCallback(handle_, std::move(callback));
    }
    VulkanShared::ScopedHandle<void()> OnStarted(std::function<void()> callback) {
        if (system_ == nullptr) {
            return {};
        }
        return system_->RegisterStartedCallback(handle_, std::move(callback));
    }
    VulkanShared::ScopedHandle<void()> OnEnded(std::function<void()> callback) {
        if (system_ == nullptr) {
            return {};
        }
        return system_->RegisterEndedCallback(handle_, std::move(callback));
    }

private:
    InputSystem* system_ = nullptr;
    ActionHandle handle_{};
};

template <std::size_t D>
Action<D> InputSystem::BindAction(std::string_view name, const ActionConfig& config) {
    static_assert(D >= 1 && D <= kMaxActionComponents, "action dimension must be within [1, kMaxActionComponents]");
    ActionConfig effective = config;
    effective.dimension = static_cast<std::uint8_t>(D);
    const ActionHandle handle = BindAction(name, effective);
    return Action<D>{this, handle};
}

}  // namespace VulkanEngine::Input
