// print_bot core: event bus. Core state changes are announced as events; consumers
// (MCP server, UI) re-read the relevant snapshot. GUI-free (no wxWidgets).
#ifndef printbot_core_EventBus_hpp_
#define printbot_core_EventBus_hpp_

#include <any>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Executor.hpp"

namespace printbot::core {

enum class Topic { Project, Presets, Slice, Job, Log, Printers, Printer };
const char *topic_name(Topic topic);

struct Event
{
    Topic          topic = Topic::Log;
    uint64_t       seq   = 0;   // assigned by the bus, strictly increasing
    std::string    key;         // what happened, e.g. "slice.progress", "project.changed"
    nlohmann::json detail;      // small JSON hint (serializable, shown to MCP clients)
    std::any       payload;     // optional typed in-process payload (e.g. exception_ptr for the GUI adapter)

    template<class T> const T *payload_as() const { return std::any_cast<T>(&payload); }
};

class EventBus
{
    struct Entry;
    struct State;

public:
    // RAII handle; destroying or reset()ing it unsubscribes. After reset() returns,
    // the callback is never invoked again (also for already-dispatched deliveries).
    class Subscription
    {
    public:
        Subscription() = default;
        ~Subscription() { reset(); }
        Subscription(Subscription &&rhs) noexcept : m_entry(std::move(rhs.m_entry)), m_state(std::move(rhs.m_state)) {}
        Subscription &operator=(Subscription &&rhs) noexcept
        {
            if (this != &rhs) { reset(); m_entry = std::move(rhs.m_entry); m_state = std::move(rhs.m_state); }
            return *this;
        }
        Subscription(const Subscription &)            = delete;
        Subscription &operator=(const Subscription &) = delete;
        void reset();
        bool active() const { return m_entry != nullptr; }

    private:
        friend class EventBus;
        std::shared_ptr<Entry> m_entry;
        std::weak_ptr<State>   m_state;
    };

    using Callback = std::function<void(const Event &)>;

    EventBus();
    ~EventBus();

    // topics empty = all topics. dispatcher empty = deliver inline on the publishing thread.
    Subscription subscribe(std::vector<Topic> topics, Callback cb, Dispatcher dispatcher = {});

    // Thread-safe. Returns the assigned sequence number.
    uint64_t publish(Event ev);
    uint64_t publish(Topic topic, std::string key, nlohmann::json detail = nlohmann::json::object(), std::any payload = {});

    // Recent history for polling clients (payloads are not kept). Events with seq > since.
    std::vector<Event> history_since(uint64_t since, size_t limit = 200) const;
    uint64_t           last_seq() const;

private:
    std::shared_ptr<State> m_state;
};

using Subscription = EventBus::Subscription;

} // namespace printbot::core

#endif
