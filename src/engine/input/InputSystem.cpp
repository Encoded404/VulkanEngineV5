module;


module VulkanEngine.Input;

import std;

namespace VulkanEngine::Input {

namespace {

[[nodiscard]] bool HasBinding(const std::unordered_set<std::int32_t>& values, std::int32_t code) {
    return values.contains(code);
}

// Every binding must drive at least one component; default to component 0.
[[nodiscard]] InputBinding NormalizeBinding(InputBinding binding) {
    if (binding.contribution_count == 0) {
        binding.contributions[0] = BindingContribution{.component = 0, .scale = 1.0f};
        binding.contribution_count = 1;
    }
    return binding;
}

}  // namespace

InputBinding InputBinding::Key(const std::int32_t keycode) {
    return InputBinding{.source = BindingSource::Key, .code = keycode};
}

InputBinding InputBinding::MouseButton(const std::int32_t button) {
    return InputBinding{.source = BindingSource::MouseButton, .code = button};
}

InputBinding InputBinding::GamepadButton(const std::int32_t button, const std::uint8_t gamepad) {
    return InputBinding{.source = BindingSource::GamepadButton, .code = button, .gamepad = gamepad};
}

InputBinding InputBinding::GamepadAxis(const std::int32_t axis, const std::uint8_t gamepad) {
    return InputBinding{.source = BindingSource::GamepadAxis, .code = axis, .gamepad = gamepad};
}

InputBinding InputBinding::MouseAxis(const std::int32_t axis) {
    return InputBinding{.source = BindingSource::MouseAxis, .code = axis};
}

InputBinding InputBinding::MouseWheel(const std::int32_t axis) {
    return InputBinding{.source = BindingSource::MouseWheel, .code = axis};
}

InputBinding InputBinding::Component(const std::uint8_t component, const float scale) const {
    InputBinding result = *this;
    if (result.contribution_count < kMaxActionComponents) {
        result.contributions[result.contribution_count] = BindingContribution{.component = component, .scale = scale};
        ++result.contribution_count;
    }
    return result;
}

InputBinding InputBinding::Bias(const float bias) const {
    InputBinding result = *this;
    result.bias = bias;
    return result;
}

InputBinding InputBinding::Deadzone(const float deadzone) const {
    InputBinding result = *this;
    result.deadzone = deadzone;
    return result;
}

InputBinding InputBinding::OnGamepad(const std::uint8_t gamepad) const {
    InputBinding result = *this;
    result.gamepad = gamepad;
    return result;
}

void InputSystem::BeginFrame() {
    raw_state_.pressed_keys.clear();
    raw_state_.released_keys.clear();
    raw_state_.pressed_mouse_buttons.clear();
    raw_state_.released_mouse_buttons.clear();
    raw_state_.mouse_delta_x = 0.0f;
    raw_state_.mouse_delta_y = 0.0f;
    raw_state_.wheel_x = 0.0f;
    raw_state_.wheel_y = 0.0f;
    for (auto& [gamepad, state] : raw_state_.gamepads) {
        static_cast<void>(gamepad);
        state.pressed_buttons.clear();
        state.released_buttons.clear();
    }
}

void InputSystem::ProcessEvent(const VulkanBackend::Event::IEvent& event) {
    if (const auto* key_down = dynamic_cast<const VulkanBackend::Event::KeyDownEvent*>(&event)) {
        raw_state_.held_keys.insert(key_down->keycode);
        raw_state_.pressed_keys.insert(key_down->keycode);
        return;
    }

    if (const auto* key_up = dynamic_cast<const VulkanBackend::Event::KeyUpEvent*>(&event)) {
        raw_state_.held_keys.erase(key_up->keycode);
        raw_state_.released_keys.insert(key_up->keycode);
        return;
    }

    if (const auto* button_down = dynamic_cast<const VulkanBackend::Event::MouseButtonDownEvent*>(&event)) {
        raw_state_.held_mouse_buttons.insert(button_down->button);
        raw_state_.pressed_mouse_buttons.insert(button_down->button);
        raw_state_.mouse_x = static_cast<float>(button_down->x);
        raw_state_.mouse_y = static_cast<float>(button_down->y);
        return;
    }

    if (const auto* button_up = dynamic_cast<const VulkanBackend::Event::MouseButtonUpEvent*>(&event)) {
        raw_state_.held_mouse_buttons.erase(button_up->button);
        raw_state_.released_mouse_buttons.insert(button_up->button);
        raw_state_.mouse_x = static_cast<float>(button_up->x);
        raw_state_.mouse_y = static_cast<float>(button_up->y);
        return;
    }

    if (const auto* motion = dynamic_cast<const VulkanBackend::Event::MouseMotionEvent*>(&event)) {
        raw_state_.mouse_x = motion->x;
        raw_state_.mouse_y = motion->y;
        raw_state_.mouse_delta_x += motion->delta_x;
        raw_state_.mouse_delta_y += motion->delta_y;
        return;
    }

    if (const auto* wheel = dynamic_cast<const VulkanBackend::Event::MouseWheelEvent*>(&event)) {
        raw_state_.wheel_x += wheel->x;
        raw_state_.wheel_y += wheel->y;
        return;
    }

    if (const auto* connected = dynamic_cast<const VulkanBackend::Event::GamepadConnectedEvent*>(&event)) {
        raw_state_.gamepads.try_emplace(connected->gamepad);
        return;
    }

    if (const auto* disconnected = dynamic_cast<const VulkanBackend::Event::GamepadDisconnectedEvent*>(&event)) {
        raw_state_.gamepads.erase(disconnected->gamepad);
        return;
    }

    if (const auto* button_down = dynamic_cast<const VulkanBackend::Event::GamepadButtonDownEvent*>(&event)) {
        GamepadRawState& gamepad = raw_state_.gamepads[button_down->gamepad];
        gamepad.held_buttons.insert(button_down->button);
        gamepad.pressed_buttons.insert(button_down->button);
        return;
    }

    if (const auto* button_up = dynamic_cast<const VulkanBackend::Event::GamepadButtonUpEvent*>(&event)) {
        GamepadRawState& gamepad = raw_state_.gamepads[button_up->gamepad];
        gamepad.held_buttons.erase(button_up->button);
        gamepad.released_buttons.insert(button_up->button);
        return;
    }

    if (const auto* axis = dynamic_cast<const VulkanBackend::Event::GamepadAxisMotionEvent*>(&event)) {
        raw_state_.gamepads[axis->gamepad].axes[axis->axis] = axis->value;
    }
}

void InputSystem::ProcessEvents(const VulkanBackend::Event::EventList& events) {
    BeginFrame();
    for (const auto& event : events) {
        if (event) {
            ProcessEvent(*event);
        }
    }
    Update();
}

bool InputSystem::SampleBinding(const InputBinding& binding, float& out) const {
    switch (binding.source) {
        case BindingSource::Key:
            out = HasBinding(raw_state_.held_keys, binding.code) ? 1.0f : 0.0f;
            return true;
        case BindingSource::MouseButton:
            out = HasBinding(raw_state_.held_mouse_buttons, binding.code) ? 1.0f : 0.0f;
            return true;
        case BindingSource::GamepadButton: {
            bool held = false;
            if (binding.gamepad == kAnyGamepad) {
                for (const auto& [gamepad, state] : raw_state_.gamepads) {
                    static_cast<void>(gamepad);
                    held = held || state.held_buttons.contains(binding.code);
                }
            } else if (const auto it = raw_state_.gamepads.find(binding.gamepad); it != raw_state_.gamepads.end()) {
                held = it->second.held_buttons.contains(binding.code);
            }
            out = held ? 1.0f : 0.0f;
            return true;
        }
        case BindingSource::GamepadAxis: {
            out = 0.0f;
            if (binding.gamepad == kAnyGamepad) {
                for (const auto& [gamepad, state] : raw_state_.gamepads) {
                    static_cast<void>(gamepad);
                    const auto axis = state.axes.find(binding.code);
                    if (axis != state.axes.end() && std::abs(axis->second) > std::abs(out)) {
                        out = axis->second;
                    }
                }
            } else if (const auto it = raw_state_.gamepads.find(binding.gamepad); it != raw_state_.gamepads.end()) {
                const auto axis = it->second.axes.find(binding.code);
                out = axis != it->second.axes.end() ? axis->second : 0.0f;
            }
            return true;
        }
        case BindingSource::MouseAxis:
            out = binding.code == 0 ? raw_state_.mouse_delta_x : raw_state_.mouse_delta_y;
            return true;
        case BindingSource::MouseWheel:
            out = binding.code == 0 ? raw_state_.wheel_x : raw_state_.wheel_y;
            return true;
    }

    out = 0.0f;
    return false;
}

void InputSystem::Update() {
    for (auto& [handle, state] : action_states_) {
        static_cast<void>(handle);
        state.started = false;
        state.canceled = false;
    }

    for (const auto& [handle, action_bindings] : bindings_) {
        const auto config_it = action_configs_.find(handle);
        const ActionConfig config = config_it != action_configs_.end() ? config_it->second : ActionConfig{};
        const std::uint8_t dimension = std::clamp<std::uint8_t>(
            config.dimension, 1, static_cast<std::uint8_t>(kMaxActionComponents));

        std::array<float, kMaxActionComponents> value{};
        for (const auto& binding : action_bindings) {
            float sample = 0.0f;
            if (!SampleBinding(binding, sample)) {
                continue;
            }
            if (binding.deadzone > 0.0f && std::abs(sample) < binding.deadzone) {
                sample = 0.0f;
            }
            sample += binding.bias;
            for (std::uint8_t i = 0; i < binding.contribution_count; ++i) {
                const BindingContribution& contribution = binding.contributions[i];
                if (contribution.component >= dimension) {
                    continue;
                }
                value[contribution.component] += sample * contribution.scale;
            }
        }

        float length_sq = 0.0f;
        for (std::uint8_t i = 0; i < dimension; ++i) {
            length_sq += value[i] * value[i];
        }
        float length = std::sqrt(length_sq);

        if (config.deadzone > 0.0f && length < config.deadzone) {
            value.fill(0.0f);
            length = 0.0f;
        }

        switch (config.processing) {
            case ActionProcessing::Direct:
                break;
            case ActionProcessing::ClampLength:
                if (length > config.max_length && length > 0.0f) {
                    const float scale = config.max_length / length;
                    for (std::uint8_t i = 0; i < dimension; ++i) {
                        value[i] *= scale;
                    }
                    length = config.max_length;
                }
                break;
            case ActionProcessing::Normalize:
                if (length > 1e-6f) {
                    const float scale = 1.0f / length;
                    for (std::uint8_t i = 0; i < dimension; ++i) {
                        value[i] *= scale;
                    }
                    length = 1.0f;
                } else {
                    value.fill(0.0f);
                    length = 0.0f;
                }
                break;
        }

        ActionState& current_state = action_states_[handle];
        const bool was_active = current_state.active;
        const bool is_active = was_active ? (length > config.release_threshold)
                                          : (length > config.press_threshold);
        current_state.active = is_active;
        current_state.started = is_active && !was_active;
        current_state.canceled = !is_active && was_active;
        current_state.values = value;
        current_state.dimension = dimension;
        current_state.value = value[0];
        current_state.value_x = value[0];
        current_state.value_y = dimension > 1 ? value[1] : 0.0f;

        auto cb_it = action_callbacks_.find(handle);
        if (cb_it != action_callbacks_.end()) {
            if (current_state.started) {
                cb_it->second.on_started.Call();
            }
            if (current_state.canceled) {
                cb_it->second.on_ended.Call();
            }
            if (current_state.active) {
                cb_it->second.on_active.Call();
            }
        }
    }
}

ActionHandle InputSystem::BindAction(std::string_view name, InputBinding binding) {
    ActionHandle handle{.id = next_action_id_++};
    bindings_[handle].push_back(NormalizeBinding(std::move(binding)));
    action_names_[handle] = std::string(name);
    action_configs_[handle] = ActionConfig{};
    action_states_[handle] = ActionState{};
    action_callbacks_[handle] = ActionCallbacks{};
    return handle;
}

ActionHandle InputSystem::BindAction(std::string_view name, const ActionConfig& config) {
    ActionHandle handle{.id = next_action_id_++};
    bindings_[handle];
    action_names_[handle] = std::string(name);
    action_configs_[handle] = config;
    action_states_[handle] = ActionState{};
    action_callbacks_[handle] = ActionCallbacks{};
    return handle;
}

void InputSystem::AddBinding(ActionHandle handle, InputBinding binding) {
    const auto it = bindings_.find(handle);
    if (it == bindings_.end()) {
        return;
    }
    it->second.push_back(NormalizeBinding(std::move(binding)));
}

void InputSystem::UnbindAction(ActionHandle handle) {
    bindings_.erase(handle);
    action_configs_.erase(handle);
    action_states_.erase(handle);
    action_names_.erase(handle);
    action_callbacks_.erase(handle);
}

void InputSystem::ClearBindings() {
    bindings_.clear();
    action_configs_.clear();
    action_states_.clear();
    action_names_.clear();
    action_callbacks_.clear();
    next_action_id_ = 0;
}

const RawInputState& InputSystem::GetRawState() const noexcept {
    return raw_state_;
}

const ActionState& InputSystem::GetActionState(ActionHandle handle) const {
    static const ActionState empty_state{};
    const auto it = action_states_.find(handle);
    if (it == action_states_.end()) {
        return empty_state;
    }
    return it->second;
}

bool InputSystem::IsActionActive(ActionHandle handle) const {
    return GetActionState(handle).active;
}

bool InputSystem::WasActionStarted(ActionHandle handle) const {
    return GetActionState(handle).started;
}

bool InputSystem::WasActionCanceled(ActionHandle handle) const {
    return GetActionState(handle).canceled;
}

float InputSystem::GetActionValue(ActionHandle handle) const {
    return GetActionState(handle).value;
}

std::pair<float, float> InputSystem::GetActionValue2D(ActionHandle handle) const {
    const auto& state = GetActionState(handle);
    return {state.value_x, state.value_y};
}

std::span<const float> InputSystem::GetActionVector(ActionHandle handle) const {
    const ActionState& state = GetActionState(handle);
    return std::span<const float>(state.values.data(), state.dimension);
}

bool InputSystem::IsGamepadConnected(std::uint8_t gamepad) const {
    return raw_state_.gamepads.contains(gamepad);
}

VulkanShared::ScopedHandle<void()> InputSystem::RegisterActiveCallback(ActionHandle handle, std::function<void()> callback) {
    return action_callbacks_[handle].on_active.Register(std::move(callback));
}

VulkanShared::ScopedHandle<void()> InputSystem::RegisterStartedCallback(ActionHandle handle, std::function<void()> callback) {
    return action_callbacks_[handle].on_started.Register(std::move(callback));
}

VulkanShared::ScopedHandle<void()> InputSystem::RegisterEndedCallback(ActionHandle handle, std::function<void()> callback) {
    return action_callbacks_[handle].on_ended.Register(std::move(callback));
}

const std::unordered_map<ActionHandle, std::string>& InputSystem::GetAllActions() const {
    return action_names_;
}

const ActionConfig& InputSystem::GetActionConfig(ActionHandle handle) const {
    static const ActionConfig empty_config{};
    const auto it = action_configs_.find(handle);
    if (it == action_configs_.end()) {
        return empty_config;
    }
    return it->second;
}

}  // namespace VulkanEngine::Input
