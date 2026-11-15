#include "EventBus.hpp"

#include <algorithm>

namespace printbot::core {

const char *topic_name(Topic topic)
{
    switch (topic) {
    case Topic::Project: return "project";
    case Topic::Presets: return "presets";
    case Topic::Slice: return "slice";
    case Topic::Job: return "job";
    case Topic::Log: return "log";
    case Topic::Printers: return "printers";
    case Topic::Printer: return "printer";
    }
    return "unknown";
}

struct EventBus::Entry
{
    std::vector<Topic>   topics;
    Callback             cb;
    Dispatcher           dispatcher;
    std::recursive_mutex mutex; // held while the callback runs; reset() waits on it
    bool                 alive = true;

    bool wants(Topic t) const { return topics.empty() || std::find(topics.begin(), topics.end(), t) != topics.end(); }
    void deliver(const Event &ev)
    {
        std::lock_guard<std::recursive_mutex> lk(mutex);
        if (alive && cb) cb(ev);
    }
};

struct EventBus::State
{
    mutable std::mutex                  mutex;
    std::vector<std::shared_ptr<Entry>> entries;
    uint64_t                            seq = 0;
    std::deque<Event>                   history;
    static constexpr size_t             history_max = 512;
};

void EventBus::Subscription::reset()
{
    if (!m_entry) return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_entry->mutex);
        m_entry->alive = false;
        m_entry->cb    = nullptr;
    }
    if (auto st = m_state.lock()) {
        std::lock_guard<std::mutex> lk(st->mutex);
        auto &v = st->entries;
        v.erase(std::remove(v.begin(), v.end(), m_entry), v.end());
    }
    m_entry.reset();
    m_state.reset();
}

EventBus::EventBus() : m_state(std::make_shared<State>()) {}
EventBus::~EventBus() = default;

EventBus::Subscription EventBus::subscribe(std::vector<Topic> topics, Callback cb, Dispatcher dispatcher)
{
    auto entry        = std::make_shared<Entry>();
    entry->topics     = std::move(topics);
    entry->cb         = std::move(cb);
    entry->dispatcher = std::move(dispatcher);
    {
        std::lock_guard<std::mutex> lk(m_state->mutex);
        m_state->entries.push_back(entry);
    }
    Subscription sub;
    sub.m_entry = std::move(entry);
    sub.m_state = m_state;
    return sub;
}

uint64_t EventBus::publish(Event ev)
{
    std::vector<std::shared_ptr<Entry>> targets;
    {
        std::lock_guard<std::mutex> lk(m_state->mutex);
        ev.seq = ++m_state->seq;
        Event hist;
        hist.topic  = ev.topic;
        hist.seq    = ev.seq;
        hist.key    = ev.key;
        hist.detail = ev.detail;
        m_state->history.push_back(std::move(hist));
        while (m_state->history.size() > State::history_max) m_state->history.pop_front();
        for (auto &e : m_state->entries)
            if (e->wants(ev.topic)) targets.push_back(e);
    }
    const uint64_t seq    = ev.seq;
    auto           shared = std::make_shared<const Event>(std::move(ev));
    for (auto &e : targets) {
        if (e->dispatcher)
            e->dispatcher([e, shared]() { e->deliver(*shared); });
        else
            e->deliver(*shared);
    }
    return seq;
}

uint64_t EventBus::publish(Topic topic, std::string key, nlohmann::json detail, std::any payload)
{
    Event ev;
    ev.topic   = topic;
    ev.key     = std::move(key);
    ev.detail  = std::move(detail);
    ev.payload = std::move(payload);
    return publish(std::move(ev));
}

std::vector<Event> EventBus::history_since(uint64_t since, size_t limit) const
{
    std::vector<Event>          out;
    std::lock_guard<std::mutex> lk(m_state->mutex);
    for (const Event &e : m_state->history)
        if (e.seq > since) {
            out.push_back(e);
            if (out.size() >= limit) break;
        }
    return out;
}

uint64_t EventBus::last_seq() const
{
    std::lock_guard<std::mutex> lk(m_state->mutex);
    return m_state->seq;
}

} // namespace printbot::core
