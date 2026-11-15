// print_bot core: executors. All core state mutations run on one executor
// (the "core thread"). GUI-free (no wxWidgets).
#ifndef printbot_core_Executor_hpp_
#define printbot_core_Executor_hpp_

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

namespace printbot::core {

using Task = std::function<void()>;
// Where a callback should run. An empty Dispatcher means "inline, on the calling thread".
// The wx UI passes one that does CallAfter(); the MCP server uses inline delivery.
using Dispatcher = std::function<void(Task)>;

class Executor
{
public:
    virtual ~Executor() = default;
    virtual void post(Task task)            = 0;
    virtual bool in_executor_thread() const = 0;

    // Run `f` on the executor and wait for its result. Runs inline when already on it.
    // Exceptions thrown by `f` are rethrown to the caller.
    template<class F> auto call(F &&f) -> std::invoke_result_t<F>
    {
        using R = std::invoke_result_t<F>;
        if (in_executor_thread())
            return f();
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut  = task->get_future();
        post([task]() { (*task)(); });
        return fut.get();
    }
};

// Owns one std::thread with a FIFO task queue.
class ThreadExecutor : public Executor
{
public:
    explicit ThreadExecutor(std::string name);
    ~ThreadExecutor() override;
    void post(Task task) override;
    bool in_executor_thread() const override;
    // Drain remaining tasks and join. Tasks posted afterwards run inline.
    void stop();

private:
    void run();

    std::string             m_name;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::deque<Task>        m_queue;
    bool                    m_stop = false;
    std::thread             m_thread;
    std::thread::id         m_thread_id;
};

// Runs tasks through a host loop (e.g. the wx main thread). Used while the GUI
// still owns the model: the "core thread" is then the host's main thread.
class DispatchExecutor : public Executor
{
public:
    DispatchExecutor(Dispatcher post, std::function<bool()> is_current) : m_post(std::move(post)), m_is_current(std::move(is_current)) {}
    void post(Task task) override
    {
        if (m_post) m_post(std::move(task));
        else task();
    }
    bool in_executor_thread() const override { return !m_is_current || m_is_current(); }

private:
    Dispatcher            m_post;
    std::function<bool()> m_is_current;
};

} // namespace printbot::core

#endif
