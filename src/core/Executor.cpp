#include "Executor.hpp"

#ifdef __linux__
#include <pthread.h>
#endif

namespace printbot::core {

ThreadExecutor::ThreadExecutor(std::string name) : m_name(std::move(name))
{
    std::promise<void> started;
    auto               started_f = started.get_future();
    m_thread                     = std::thread([this, &started]() {
        m_thread_id = std::this_thread::get_id();
#ifdef __linux__
        pthread_setname_np(pthread_self(), m_name.substr(0, 15).c_str());
#endif
        started.set_value();
        run();
    });
    started_f.wait();
}

ThreadExecutor::~ThreadExecutor() { stop(); }

void ThreadExecutor::post(Task task)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_stop) {
            m_queue.emplace_back(std::move(task));
            m_cv.notify_one();
            return;
        }
    }
    // Stopped: never drop work silently (a blocked call() would hang).
    task();
}

bool ThreadExecutor::in_executor_thread() const { return std::this_thread::get_id() == m_thread_id; }

void ThreadExecutor::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_stop) return;
        m_stop = true;
        m_cv.notify_all();
    }
    if (m_thread.joinable()) {
        if (std::this_thread::get_id() == m_thread.get_id()) m_thread.detach();
        else m_thread.join();
    }
}

void ThreadExecutor::run()
{
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this]() { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) return; // stopped and drained
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            task();
        } catch (...) {
            // A posted task must not take the core thread down; call() reports
            // exceptions through its future instead.
        }
    }
}

} // namespace printbot::core
