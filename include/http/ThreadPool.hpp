#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace http {

// A fixed set of worker threads fed from a bounded queue. Tasks are
// move-only, so a task can own a resource such as a client socket.
class ThreadPool {
public:
    using Task = std::move_only_function<void()>;

    // Starts `threadCount` workers. At most `queueCapacity` tasks wait for a
    // free worker. Throws std::invalid_argument if threadCount is 0.
    ThreadPool(std::size_t threadCount, std::size_t queueCapacity);

    // Runs every task already queued, then joins the workers.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Queues `task`, or returns false and drops it if the queue is full.
    [[nodiscard]] bool trySubmit(Task task);

private:
    void work();
    void stop() noexcept;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> queue_;
    std::size_t capacity_;
    bool stopping_{false};
    // Declared last so the threads are joined before the members they use
    // are destroyed.
    std::vector<std::jthread> workers_;
};

} // namespace http
