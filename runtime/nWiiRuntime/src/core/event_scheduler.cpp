#include "runtime/event_scheduler.h"
#include "runtime/cpu_context.h"
#include <algorithm>

namespace nwii::runtime {

// R9 guestloop: constant-initialized (std::vector's default constructor is
// constexpr), so it exists before any dynamic initializer runs.
constinit EventScheduler g_event_scheduler;

void EventScheduler::sift_up(size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (m_heap[parent].due < m_heap[i].due ||
            (m_heap[parent].due == m_heap[i].due && m_heap[parent].id <= m_heap[i].id))
            break;
        std::swap(m_heap[parent], m_heap[i]);
        i = parent;
    }
}

void EventScheduler::sift_down(size_t i) {
    size_t n = m_heap.size();
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, best = i;
        auto before = [&](size_t a, size_t b) {
            return m_heap[a].due < m_heap[b].due ||
                   (m_heap[a].due == m_heap[b].due && m_heap[a].id < m_heap[b].id);
        };
        if (l < n && before(l, best)) best = l;
        if (r < n && before(r, best)) best = r;
        if (best == i) break;
        std::swap(m_heap[best], m_heap[i]);
        i = best;
    }
}

extern CPUContext *g_ctx_ptr;  // main.cpp

void EventScheduler::push(Event e) {
    // R9 guestloop: an event that is already due fires at the next poll, as
    // in r8 (the poll's countdown only covers clock refreshes).
    if (e.due <= m_now)
        if (CPUContext *c = g_ctx_ptr) c->poll_kick();
    m_heap.push_back(std::move(e));
    sift_up(m_heap.size() - 1);
    update_next_due();
}

EventScheduler::EventId EventScheduler::schedule_after(uint64_t delay, Callback cb) {
    EventId id = m_next_id++;
    push({m_now + delay, id, 0, std::move(cb)});
    return id;
}

EventScheduler::EventId EventScheduler::schedule_recurring(uint64_t period, Callback cb) {
    EventId id = m_next_id++;
    uint64_t p = period ? period : 1;
    push({m_now + p, id, p, std::move(cb)});
    return id;
}

bool EventScheduler::cancel(EventId id) {
    for (size_t i = 0; i < m_heap.size(); ++i) {
        if (m_heap[i].id == id) {
            m_heap[i] = std::move(m_heap.back());
            m_heap.pop_back();
            if (i < m_heap.size()) {
                sift_up(i);
                sift_down(i);
            }
            update_next_due();
            return true;
        }
    }
    return false;
}

// Slow half of advance() (event_scheduler.h): m_now is already set and at
// least one event is due.
void EventScheduler::fire_due(CPUContext& ctx) {
    m_firing = true;

    
    while (!m_heap.empty() && m_heap.front().due <= m_now) {
        Event e = std::move(m_heap.front());
        m_heap[0] = std::move(m_heap.back());
        m_heap.pop_back();
        if (!m_heap.empty()) sift_down(0);
        update_next_due();

        uint64_t late = m_now - e.due;
        if (e.period) {

            uint64_t next = e.due + e.period;
            if (next <= m_now) next = m_now + e.period;
            Callback cb = e.cb; 
            push({next, e.id, e.period, cb});
            cb(ctx, late);
        } else {
            e.cb(ctx, late);
        }
    }
    m_firing = false;
}

void EventScheduler::reset() {
    m_heap.clear();
    m_next_due = UINT64_MAX;
    m_now = 0;
    m_next_id = 1;
    m_firing = false;
}

} 
