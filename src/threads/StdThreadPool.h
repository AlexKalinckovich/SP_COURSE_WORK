#pragma once

#include "IThreadManager.h"

#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <memory>

class StdThreadPool final : public IThreadManager
{
public:
    /**
     * Create a thread pool.
     * If numThreads == 0 the pool will use std::thread::hardware_concurrency() or 1.
     */
    explicit StdThreadPool(std::size_t numThreads = 0);

    ~StdThreadPool() override;

    StdThreadPool(const StdThreadPool&) = delete;
    StdThreadPool& operator=(const StdThreadPool&) = delete;

    void enqueue(Task task) override;

    RecurringId scheduleRecurringGeneric(std::chrono::milliseconds interval, Task task) override;

    void cancelRecurring(RecurringId id) override;

    void shutdown(bool graceful) override;

private:
    void workerLoop(std::stop_token stoken);

    std::size_t m_numThreads;
    std::vector<std::jthread> m_workers;
    std::deque<Task> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stopping{ false };

    struct TimerRecord
    {
        std::shared_ptr<std::atomic_bool> cancelled;
        std::jthread thread;
    };

    std::unordered_map<RecurringId, TimerRecord> m_timers;
    std::atomic<RecurringId> m_nextRecurringId{ 1 };
};
