module;

export module VulkanBackend.Event;

import std;

export namespace VulkanEngine::Backend::Event {

enum class EventCategory : std::uint8_t {
    System,
    Platform,
    Input
};

enum class EventType : std::uint8_t {
    Quit,
    WindowResized,
    WindowMinimized,
    WindowRestored,
    KeyDown,
    KeyUp,
    MouseButtonDown,
    MouseButtonUp,
    MouseMotion,
    MouseWheel
};

class IEvent {
public:
    virtual ~IEvent() = default;

    [[nodiscard]] virtual std::type_index GetType() const noexcept = 0;
    [[nodiscard]] virtual EventCategory GetCategory() const noexcept = 0;
    [[nodiscard]] virtual EventType GetEventType() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<IEvent> Clone() const = 0;
};

class EventBase : public IEvent {
public:
    EventBase(std::type_index type, EventCategory category, EventType event_type) noexcept;
    EventBase(const EventBase&) = default;
    EventBase& operator=(const EventBase&) = default;
    EventBase(EventBase&&) noexcept = default;
    EventBase& operator=(EventBase&&) noexcept = default;
    ~EventBase() override = default;

    [[nodiscard]] std::type_index GetType() const noexcept final;
    [[nodiscard]] EventCategory GetCategory() const noexcept final;
    [[nodiscard]] EventType GetEventType() const noexcept override;
    [[nodiscard]] std::unique_ptr<IEvent> Clone() const final;

protected:
    [[nodiscard]] virtual std::unique_ptr<IEvent> CloneEvent() const = 0;

private:
    std::type_index type_;
    EventCategory category_;
    EventType event_type_;
};

using EventPtr = std::unique_ptr<IEvent>;
using EventList = std::vector<EventPtr>;

class QuitEvent final : public EventBase {
public:
    QuitEvent();

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::Quit; }

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class WindowResizedEvent final : public EventBase {
public:
    WindowResizedEvent(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::WindowResized; }

    std::uint32_t width = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t height = 0; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class WindowMinimizedEvent final : public EventBase {
public:
    WindowMinimizedEvent();

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::WindowMinimized; }

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class WindowRestoredEvent final : public EventBase {
public:
    WindowRestoredEvent();

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::WindowRestored; }

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class KeyDownEvent final : public EventBase {
public:
    KeyDownEvent(std::int32_t keycode, bool repeat = false);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::KeyDown; }

    std::int32_t keycode = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    bool repeat = false; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class KeyUpEvent final : public EventBase {
public:
    explicit KeyUpEvent(std::int32_t keycode);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::KeyUp; }

    std::int32_t keycode = 0; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class MouseButtonDownEvent final : public EventBase {
public:
    MouseButtonDownEvent(std::int32_t button, std::int32_t x, std::int32_t y);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::MouseButtonDown; }

    std::int32_t button = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::int32_t x = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::int32_t y = 0; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class MouseButtonUpEvent final : public EventBase {
public:
    MouseButtonUpEvent(std::int32_t button, std::int32_t x, std::int32_t y);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::MouseButtonUp; }

    std::int32_t button = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::int32_t x = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::int32_t y = 0; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class MouseMotionEvent final : public EventBase {
public:
    MouseMotionEvent(float x, float y, float delta_x, float delta_y);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::MouseMotion; }

    float x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float delta_x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float delta_y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class MouseWheelEvent final : public EventBase {
public:
    MouseWheelEvent(float x, float y);

    [[nodiscard]] EventType GetEventType() const noexcept override { return EventType::MouseWheel; }

    float x = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float y = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::unique_ptr<IEvent> CloneEvent() const override;
};

class EventQueue {
public:
    void Push(EventPtr event);
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    void Clear() noexcept;
    [[nodiscard]] EventList Drain();

private:
    EventList events_{};
};

}  // namespace VulkanEngine::Backend::Event

