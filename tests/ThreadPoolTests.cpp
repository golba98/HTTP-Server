#include "http/ThreadPool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

using http::ThreadPool;
using namespace std::chrono_literals;

namespace {

// A one-shot signal that a task can wait on without hanging the test forever.
class Gate {
public:
    void open()
    {
        {
            const std::scoped_lock lock{mutex_};
            open_ = true;
        }
        opened_.notify_all();
    }

    // Returns false if the gate stayed shut for the whole timeout.
    bool waitFor(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return opened_.wait_for(lock, timeout, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable opened_;
    bool open_{false};
};

} // namespace

TEST_CASE("ThreadPool runs every submitted task", "[ThreadPool]")
{
    std::atomic<int> runs{0};
    {
        ThreadPool pool{4, 100};
        for (int i = 0; i < 100; ++i) {
            CHECK(pool.trySubmit([&runs] { ++runs; }));
        }
    }

    CHECK(runs == 100);
}

TEST_CASE("ThreadPool runs tasks on all of its threads at once", "[ThreadPool]")
{
    constexpr int threads = 4;
    std::mutex mutex;
    std::condition_variable arrived;
    int arrivals = 0;
    std::atomic<int> sawEveryone{0};
    {
        ThreadPool pool{threads, threads};
        for (int i = 0; i < threads; ++i) {
            REQUIRE(pool.trySubmit([&] {
                std::unique_lock lock{mutex};
                ++arrivals;
                arrived.notify_all();
                // Only possible if every task is running at the same time.
                if (arrived.wait_for(lock, 5s, [&] { return arrivals == threads; })) {
                    ++sawEveryone;
                }
            }));
        }
    }

    CHECK(sawEveryone == threads);
}

TEST_CASE("ThreadPool rejects tasks when its queue is full", "[ThreadPool]")
{
    Gate release;
    std::promise<void> started;
    std::atomic<bool> queuedRan{false};
    std::atomic<bool> rejectedRan{false};
    {
        ThreadPool pool{1, 1};
        REQUIRE(pool.trySubmit([&] {
            started.set_value();
            release.waitFor(5s);
        }));
        // Once the only worker is busy, one task fits in the queue.
        REQUIRE(started.get_future().wait_for(5s) == std::future_status::ready);

        CHECK(pool.trySubmit([&] { queuedRan = true; }));
        CHECK_FALSE(pool.trySubmit([&] { rejectedRan = true; }));

        release.open();
    }

    CHECK(queuedRan);
    CHECK_FALSE(rejectedRan);
}

TEST_CASE("ThreadPool leaves a rejected task with the caller", "[ThreadPool]")
{
    Gate release;
    std::promise<void> started;
    ThreadPool pool{1, 1};
    REQUIRE(pool.trySubmit([&] {
        started.set_value();
        release.waitFor(5s);
    }));
    REQUIRE(started.get_future().wait_for(5s) == std::future_status::ready);
    REQUIRE(pool.trySubmit([] { }));

    bool ran = false;
    ThreadPool::Task task = [&ran] { ran = true; };
    CHECK_FALSE(pool.trySubmit(std::move(task)));

    // The caller still owns the task, and whatever it captured.
    REQUIRE(static_cast<bool>(task));
    task();
    CHECK(ran);

    release.open();
}

TEST_CASE("ThreadPool destructor finishes the queued tasks", "[ThreadPool]")
{
    Gate release;
    std::atomic<int> runs{0};
    {
        ThreadPool pool{1, 10};
        REQUIRE(pool.trySubmit([&] { release.waitFor(5s); }));
        for (int i = 0; i < 5; ++i) {
            REQUIRE(pool.trySubmit([&runs] { ++runs; }));
        }
        release.open();
    }

    CHECK(runs == 5);
}

TEST_CASE("ThreadPool accepts move-only tasks", "[ThreadPool]")
{
    std::promise<int> result;
    auto future = result.get_future();
    {
        ThreadPool pool{1, 1};
        auto value = std::make_unique<int>(42);
        REQUIRE(pool.trySubmit([value = std::move(value), &result] { result.set_value(*value); }));
    }

    CHECK(future.get() == 42);
}

TEST_CASE("ThreadPool keeps working after a task throws", "[ThreadPool]")
{
    std::atomic<bool> laterRan{false};
    {
        ThreadPool pool{1, 2};
        REQUIRE(pool.trySubmit([] { throw std::runtime_error{"expected by the test"}; }));
        REQUIRE(pool.trySubmit([&laterRan] { laterRan = true; }));
    }

    CHECK(laterRan);
}

TEST_CASE("ThreadPool needs at least one thread", "[ThreadPool]")
{
    CHECK_THROWS_AS(ThreadPool(0, 1), std::invalid_argument);
}
