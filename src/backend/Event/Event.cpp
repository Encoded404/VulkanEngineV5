module;

#include <cstdint>
#include <memory>
#include <typeindex>
//#include <typeinfo>

module VulkanBackend.Event;

namespace VulkanEngine::Backend::Event {

Event::Event(std::type_index type, EventCategory category) noexcept
    : type_(type), category_(category) {}

std::type_index Event::GetType() const noexcept {
    return type_;
}

EventCategory Event::GetCategory() const noexcept {
    return category_;
}

std::unique_ptr<IEvent> Event::Clone() const {
    return CloneEvent();
}

QuitEvent::QuitEvent()
    : Event(typeid(QuitEvent), EventCategory::Platform) {}

std::unique_ptr<IEvent> QuitEvent::CloneEvent() const {
    return std::make_unique<QuitEvent>(*this);
}

WindowResizedEvent::WindowResizedEvent(uint32_t width_value, uint32_t height_value)
    : Event(typeid(WindowResizedEvent), EventCategory::Platform), width(width_value), height(height_value) {}

std::unique_ptr<IEvent> WindowResizedEvent::CloneEvent() const {
    return std::make_unique<WindowResizedEvent>(*this);
}

WindowMinimizedEvent::WindowMinimizedEvent()
    : Event(typeid(WindowMinimizedEvent), EventCategory::Platform) {}

std::unique_ptr<IEvent> WindowMinimizedEvent::CloneEvent() const {
    return std::make_unique<WindowMinimizedEvent>(*this);
}

WindowRestoredEvent::WindowRestoredEvent()
    : Event(typeid(WindowRestoredEvent), EventCategory::Platform) {}

std::unique_ptr<IEvent> WindowRestoredEvent::CloneEvent() const {
    return std::make_unique<WindowRestoredEvent>(*this);
}

KeyDownEvent::KeyDownEvent(int32_t keycode_value, bool repeat_value)
    : Event(typeid(KeyDownEvent), EventCategory::Input), keycode(keycode_value), repeat(repeat_value) {}

std::unique_ptr<IEvent> KeyDownEvent::CloneEvent() const {
    return std::make_unique<KeyDownEvent>(*this);
}

KeyUpEvent::KeyUpEvent(int32_t keycode_value)
    : Event(typeid(KeyUpEvent), EventCategory::Input), keycode(keycode_value) {}

std::unique_ptr<IEvent> KeyUpEvent::CloneEvent() const {
    return std::make_unique<KeyUpEvent>(*this);
}

MouseButtonDownEvent::MouseButtonDownEvent(int32_t button_value, int32_t x_value, int32_t y_value)
    : Event(typeid(MouseButtonDownEvent), EventCategory::Input), button(button_value), x(x_value), y(y_value) {}

std::unique_ptr<IEvent> MouseButtonDownEvent::CloneEvent() const {
    return std::make_unique<MouseButtonDownEvent>(*this);
}

MouseButtonUpEvent::MouseButtonUpEvent(int32_t button_value, int32_t x_value, int32_t y_value)
    : Event(typeid(MouseButtonUpEvent), EventCategory::Input), button(button_value), x(x_value), y(y_value) {}

std::unique_ptr<IEvent> MouseButtonUpEvent::CloneEvent() const {
    return std::make_unique<MouseButtonUpEvent>(*this);
}

MouseMotionEvent::MouseMotionEvent(float x_value, float y_value, float delta_x_value, float delta_y_value)
    : Event(typeid(MouseMotionEvent), EventCategory::Input), x(x_value), y(y_value), delta_x(delta_x_value), delta_y(delta_y_value) {}

std::unique_ptr<IEvent> MouseMotionEvent::CloneEvent() const {
    return std::make_unique<MouseMotionEvent>(*this);
}

MouseWheelEvent::MouseWheelEvent(float x_value, float y_value)
    : Event(typeid(MouseWheelEvent), EventCategory::Input), x(x_value), y(y_value) {}

std::unique_ptr<IEvent> MouseWheelEvent::CloneEvent() const {
    return std::make_unique<MouseWheelEvent>(*this);
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

}  // namespace VulkanEngine::Backend::Event


