#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace nwii::runtime {

struct CPUContext;

// uses: Dolphin CoreTiming). Instead of scattered `inst_count % N == 0`





class EventScheduler {
public:
    using EventId = uint64_t;


    using Callback = std::function<void(CPUContext& ctx, uint64_t late_ticks)>;

    // Inline: the interrupt poll on every guest back-edge calls get() and
    // advance(); out of line they were ~12% of the CPU thread in a busy loop.
    // R9 guestloop: a namespace-scope object (constant-initialized, see
    // event_scheduler.cpp) instead of a function-local static, so get() has
    // no initialization guard (an acquire load + branch per poll on AArch64).
    static EventScheduler& get();

    EventId schedule_after(uint64_t delay, Callback cb);


    EventId schedule_recurring(uint64_t period, Callback cb);

    bool cancel(EventId id);

    // Moves the clock to `now` and fires every event due by then. The common
    // case (nothing due yet) is decided here without a call.
    // R9 guestloop: m_next_due caches m_heap.front().due (UINT64_MAX when
    // empty), so the common case is one compare.
    void advance(CPUContext& ctx, uint64_t now) {
        if (now < m_now) return;
        m_now = now;
        if (now < m_next_due || m_firing) return;
        fire_due(ctx);
    }

    void reset();
    uint64_t now() const { return m_now; }
    uint64_t next_due() const { return m_next_due; }

private:
    struct Event {
        uint64_t due;
        EventId id;
        uint64_t period;
        Callback cb;
    };

    std::vector<Event> m_heap;
    uint64_t m_now = 0;
    uint64_t m_next_due = UINT64_MAX;  // m_heap.front().due, UINT64_MAX if empty
    EventId m_next_id = 1;
    bool m_firing = false;

    void fire_due(CPUContext& ctx);
    void push(Event e);
    void sift_up(size_t i);
    void sift_down(size_t i);
    void update_next_due() { m_next_due = m_heap.empty() ? UINT64_MAX : m_heap.front().due; }
};

extern EventScheduler g_event_scheduler;  // event_scheduler.cpp
inline EventScheduler& EventScheduler::get() { return g_event_scheduler; }

}
