module;

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

module VulkanEngine.Input;

namespace VulkanEngine::Input {

namespace {

[[nodiscard]] bool HasBinding(const std::unordered_set<int32_t>& values, int32_t code) {
    return values.contains(code);
}

}  // namespace

InputBinding InputBinding::Key(int32_t keycode) {
    return InputBinding{.type = BindingType::Key, .code = keycode};
}

InputBinding InputBinding::MouseButton(int32_t button) {
    return InputBinding{.type = BindingType::MouseButton, .code = button};
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
}

void InputSystem::ProcessEvent(const VulkanEngine::Backend::Event::IEvent& event) {
    if (const auto* key_down = dynamic_cast<const VulkanEngine::Backend::Event::KeyDownEvent*>(&event)) {
        raw_state_.held_keys.insert(key_down->keycode);
        raw_state_.pressed_keys.insert(key_down->keycode);
        return;
    }

    if (const auto* key_up = dynamic_cast<const VulkanEngine::Backend::Event::KeyUpEvent*>(&event)) {
        raw_state_.held_keys.erase(key_up->keycode);
        raw_state_.released_keys.insert(key_up->keycode);
        return;
    }

    if (const auto* button_down = dynamic_cast<const VulkanEngine::Backend::Event::MouseButtonDownEvent*>(&event)) {
        raw_state_.held_mouse_buttons.insert(button_down->button);
        raw_state_.pressed_mouse_buttons.insert(button_down->button);
        raw_state_.mouse_x = static_cast<float>(button_down->x);
        raw_state_.mouse_y = static_cast<float>(button_down->y);
        return;
    }

    if (const auto* button_up = dynamic_cast<const VulkanEngine::Backend::Event::MouseButtonUpEvent*>(&event)) {
        raw_state_.held_mouse_buttons.erase(button_up->button);
        raw_state_.released_mouse_buttons.insert(button_up->button);
        raw_state_.mouse_x = static_cast<float>(button_up->x);
        raw_state_.mouse_y = static_cast<float>(button_up->y);
        return;
    }

    if (const auto* motion = dynamic_cast<const VulkanEngine::Backend::Event::MouseMotionEvent*>(&event)) {
        raw_state_.mouse_x = motion->x;
        raw_state_.mouse_y = motion->y;
        raw_state_.mouse_delta_x += motion->delta_x;
        raw_state_.mouse_delta_y += motion->delta_y;
        return;
    }

    if (const auto* wheel = dynamic_cast<const VulkanEngine::Backend::Event::MouseWheelEvent*>(&event)) {
        raw_state_.wheel_x += wheel->x;
        raw_state_.wheel_y += wheel->y;
    }
}

void InputSystem::ProcessEvents(const VulkanEngine::Backend::Event::EventList& events) {
    BeginFrame();
    for (const auto& event : events) {
        if (event) {
            ProcessEvent(*event);
        }
    }
    Update();
}

void InputSystem::Update() {
    action_states_.clear();

    for (const auto& [action, action_bindings] : bindings_) {
        ActionState state{};
        for (const auto& binding : action_bindings) {
            switch (binding.type) {
                case BindingType::Key:
                    state.active = state.active || HasBinding(raw_state_.held_keys, binding.code);
                    state.started = state.started || HasBinding(raw_state_.pressed_keys, binding.code);
                    state.canceled = state.canceled || HasBinding(raw_state_.released_keys, binding.code);
                    break;
                case BindingType::MouseButton:
                    state.active = state.active || HasBinding(raw_state_.held_mouse_buttons, binding.code);
                    state.started = state.started || HasBinding(raw_state_.pressed_mouse_buttons, binding.code);
                    state.canceled = state.canceled || HasBinding(raw_state_.released_mouse_buttons, binding.code);
                    break;
            }
        }
        action_states_[action] = state;
    }
}

void InputSystem::BindAction(ActionId action, InputBinding binding) {
    bindings_[std::move(action)].push_back(binding);
}

void InputSystem::ClearBindings() {
    bindings_.clear();
    action_states_.clear();
}

const RawInputState& InputSystem::GetRawState() const noexcept {
    return raw_state_;
}

const ActionState& InputSystem::GetActionState(std::string_view action) const {
    static const ActionState empty_state{};
    const auto it = action_states_.find(std::string(action));
    if (it == action_states_.end()) {
        return empty_state;
    }
    return it->second;
}

bool InputSystem::IsActionActive(std::string_view action) const {
    return GetActionState(action).active;
}

bool InputSystem::WasActionStarted(std::string_view action) const {
    return GetActionState(action).started;
}

bool InputSystem::WasActionCanceled(std::string_view action) const {
    return GetActionState(action).canceled;
}

}  // namespace VulkanEngine::Input


