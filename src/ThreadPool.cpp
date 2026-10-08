#include "http/ThreadPool.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace http {

ThreadPool::ThreadPool(std::size_t threadCount, std::size_t queueCapacity)
    : capacity_{queueCapacity}
{
    if (threadCount == 0) {
        throw std::invalid_argument{"ThreadPool needs at least one thread"};
    }
    workers_.reserve(threadCount);
    try {
        for (std::size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    } catch (...) {
        // The destructor will not run, so release the threads already
        // started; joining them while they wait for work would hang.
        stop();
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    stop();
    // workers_ is destroyed next, which joins every thread once the queue
    // is empty.
}

void ThreadPool::stop() noexcept
{
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
}

bool ThreadPool::trySubmit(Task&& task)
{
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    wake_.notify_one();
    return true;
}

void ThreadPool::work()
{
    for (;;) {
        Task task;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return; // Stopping, and nothing is left to run.
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        // One failing task must not take a worker down with it.
        try {
            task();
        } catch (const std::exception& error) {
            std::osyncstream{std::cerr} << "thread pool: task failed: " << error.what() << '\n';
        } catch (...) {
            std::osyncstream{std::cerr} << "thread pool: task failed with an unknown exception\n";
        }
    }
}

} // namespace http
