module;

//#include <typeinfo>

module VulkanBackend.Event;

import std;
import std.compat;

namespace VulkanBackend::Event {

EventBase::EventBase(std::type_index type, EventCategory category, EventType event_type) noexcept
    : type_(type), category_(category), event_type_(event_type) {}

std::type_index EventBase::GetType() const noexcept {
    return type_;
}

EventCategory EventBase::GetCategory() const noexcept {
    return category_;
}

EventType EventBase::GetEventType() const noexcept {
    return event_type_;
}

std::unique_ptr<IEvent> EventBase::Clone() const {
    return CloneEvent();
}

QuitEvent::QuitEvent()
    : EventBase(typeid(QuitEvent), EventCategory::Platform, EventType::Quit) {}

std::unique_ptr<IEvent> QuitEvent::CloneEvent() const {
    return std::make_unique<QuitEvent>(*this);
}

WindowResizedEvent::WindowResizedEvent(std::uint32_t width_value, std::uint32_t height_value)
    : EventBase(typeid(WindowResizedEvent), EventCategory::Platform, EventType::WindowResized), width(width_value), height(height_value) {}

std::unique_ptr<IEvent> WindowResizedEvent::CloneEvent() const {
    return std::make_unique<WindowResizedEvent>(*this);
}

WindowMinimizedEvent::WindowMinimizedEvent()
    : EventBase(typeid(WindowMinimizedEvent), EventCategory::Platform, EventType::WindowMinimized) {}

std::unique_ptr<IEvent> WindowMinimizedEvent::CloneEvent() const {
    return std::make_unique<WindowMinimizedEvent>(*this);
}

WindowRestoredEvent::WindowRestoredEvent()
    : EventBase(typeid(WindowRestoredEvent), EventCategory::Platform, EventType::WindowRestored) {}

std::unique_ptr<IEvent> WindowRestoredEvent::CloneEvent() const {
    return std::make_unique<WindowRestoredEvent>(*this);
}

KeyDownEvent::KeyDownEvent(std::int32_t keycode_value, bool repeat_value)
    : EventBase(typeid(KeyDownEvent), EventCategory::Input, EventType::KeyDown), keycode(keycode_value), repeat(repeat_value) {}

std::unique_ptr<IEvent> KeyDownEvent::CloneEvent() const {
    return std::make_unique<KeyDownEvent>(*this);
}

KeyUpEvent::KeyUpEvent(std::int32_t keycode_value)
    : EventBase(typeid(KeyUpEvent), EventCategory::Input, EventType::KeyUp), keycode(keycode_value) {}

std::unique_ptr<IEvent> KeyUpEvent::CloneEvent() const {
    return std::make_unique<KeyUpEvent>(*this);
}

MouseButtonDownEvent::MouseButtonDownEvent(std::int32_t button_value, std::int32_t x_value, std::int32_t y_value)
    : EventBase(typeid(MouseButtonDownEvent), EventCategory::Input, EventType::MouseButtonDown), button(button_value), x(x_value), y(y_value) {}

std::unique_ptr<IEvent> MouseButtonDownEvent::CloneEvent() const {
    return std::make_unique<MouseButtonDownEvent>(*this);
}

MouseButtonUpEvent::MouseButtonUpEvent(std::int32_t button_value, std::int32_t x_value, std::int32_t y_value)
    : EventBase(typeid(MouseButtonUpEvent), EventCategory::Input, EventType::MouseButtonUp), button(button_value), x(x_value), y(y_value) {}

std::unique_ptr<IEvent> MouseButtonUpEvent::CloneEvent() const {
    return std::make_unique<MouseButtonUpEvent>(*this);
}

MouseMotionEvent::MouseMotionEvent(float x_value, float y_value, float delta_x_value, float delta_y_value)
    : EventBase(typeid(MouseMotionEvent), EventCategory::Input, EventType::MouseMotion), x(x_value), y(y_value), delta_x(delta_x_value), delta_y(delta_y_value) {}

std::unique_ptr<IEvent> MouseMotionEvent::CloneEvent() const {
    return std::make_unique<MouseMotionEvent>(*this);
}

MouseWheelEvent::MouseWheelEvent(float x_value, float y_value)
    : EventBase(typeid(MouseWheelEvent), EventCategory::Input, EventType::MouseWheel), x(x_value), y(y_value) {}

std::unique_ptr<IEvent> MouseWheelEvent::CloneEvent() const {
    return std::make_unique<MouseWheelEvent>(*this);
}

GamepadConnectedEvent::GamepadConnectedEvent(std::uint8_t gamepad_index)
    : EventBase(typeid(GamepadConnectedEvent), EventCategory::Input, EventType::GamepadConnected), gamepad(gamepad_index) {}

std::unique_ptr<IEvent> GamepadConnectedEvent::CloneEvent() const {
    return std::make_unique<GamepadConnectedEvent>(*this);
}

GamepadDisconnectedEvent::GamepadDisconnectedEvent(std::uint8_t gamepad_index)
    : EventBase(typeid(GamepadDisconnectedEvent), EventCategory::Input, EventType::GamepadDisconnected), gamepad(gamepad_index) {}

std::unique_ptr<IEvent> GamepadDisconnectedEvent::CloneEvent() const {
    return std::make_unique<GamepadDisconnectedEvent>(*this);
}

GamepadButtonDownEvent::GamepadButtonDownEvent(std::uint8_t gamepad_index, std::int32_t button_index)
    : EventBase(typeid(GamepadButtonDownEvent), EventCategory::Input, EventType::GamepadButtonDown), gamepad(gamepad_index), button(button_index) {}

std::unique_ptr<IEvent> GamepadButtonDownEvent::CloneEvent() const {
    return std::make_unique<GamepadButtonDownEvent>(*this);
}

GamepadButtonUpEvent::GamepadButtonUpEvent(std::uint8_t gamepad_index, std::int32_t button_index)
    : EventBase(typeid(GamepadButtonUpEvent), EventCategory::Input, EventType::GamepadButtonUp), gamepad(gamepad_index), button(button_index) {}

std::unique_ptr<IEvent> GamepadButtonUpEvent::CloneEvent() const {
    return std::make_unique<GamepadButtonUpEvent>(*this);
}

GamepadAxisMotionEvent::GamepadAxisMotionEvent(std::uint8_t gamepad_index, std::int32_t axis_index, float axis_value)
    : EventBase(typeid(GamepadAxisMotionEvent), EventCategory::Input, EventType::GamepadAxisMotion), gamepad(gamepad_index), axis(axis_index), value(axis_value) {}

std::unique_ptr<IEvent> GamepadAxisMotionEvent::CloneEvent() const {
    return std::make_unique<GamepadAxisMotionEvent>(*this);
}

void EventQueue::Push(EventPtr event) {
    if (event) {
        events_.push_back(std::move(event));
    }
}

bool EventQueue::Empty() const noexcept {
    return events_.empty();
}

std::size_t EventQueue::Size() const noexcept {
    return events_.size();
}

void EventQueue::Clear() noexcept {
    events_.clear();
}

EventList EventQueue::Drain() {
    EventList drained{};
    drained.swap(events_);
    return drained;
}

}  // namespace VulkanBackend::Event


