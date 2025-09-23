#include "StdThreadPool.h"

#include <chrono>
#include <functional>
#include <stdexcept>

StdThreadPool::StdThreadPool(const std::size_t numThreads)
    : m_numThreads(numThreads)
{
    if (m_numThreads == 0)
    {
        const unsigned int hc = std::thread::hardware_concurrency();
        if (hc == 0)
            m_numThreads = 1u;
        else
            m_numThreads = hc;
    }

    for (std::size_t i = 0; i < m_numThreads; ++i)
    {
        m_workers.emplace_back([this](const std::stop_token &st) -> void
        {
            this->workerLoop(st);
        });
    }
}

StdThreadPool::~StdThreadPool()
{
    try
    {
        StdThreadPool::shutdown(true);
    }
    catch (...)
    {}
}

void StdThreadPool::enqueue(Task task)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping.load())
        {
            throw std::runtime_error("enqueue on stopped thread pool");
        }

        m_tasks.emplace_back(std::move(task));
    }

    m_cv.notify_one();
}

void StdThreadPool::workerLoop(std::stop_token stoken)
{
    while (!stoken.stop_requested())
    {
        Task task;

        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this, &stoken] -> bool
            {
                return m_stopping.load() || !m_tasks.empty() || stoken.stop_requested();
            });

            if (m_stopping.load() && m_tasks.empty())
            {
                return;
            }

            if (!m_tasks.empty())
            {
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            else
            {
                continue;
            }
        }

        try
        {
            task();
        }
        catch (...)
        {
        }
    }
}

IThreadManager::RecurringId
StdThreadPool::scheduleRecurringGeneric(std::chrono::milliseconds interval, Task task)
{
    RecurringId id = m_nextRecurringId.fetch_add(1);

    auto cancelled = std::make_shared<std::atomic_bool>(false);

    std::jthread timerThread([this, interval, task = std::move(task), cancelled](const std::stop_token &st)
    {
        while (!st.stop_requested() && !cancelled->load())
        {
            std::this_thread::sleep_for(interval);

            if (st.stop_requested() || cancelled->load())
            {
                break;
            }

            try
            {
                this->enqueue(task);
            }
            catch (...)
            {
                break;
            }
        }
    });

    TimerRecord rec;
    rec.cancelled = cancelled;
    rec.thread = std::move(timerThread);

    {
        std::lock_guard lock(m_mutex);
        m_timers.emplace(id, std::move(rec));
    }

    return id;
}

void
StdThreadPool::cancelRecurring(RecurringId id)
{
    std::lock_guard lock(m_mutex);
    const auto it = m_timers.find(id);
    if (it != m_timers.end())
    {
        it->second.cancelled->store(true);
        it->second.thread.request_stop();
        m_timers.erase(it);
    }
}

void StdThreadPool::shutdown(const bool graceful)
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping.store(true);

        if (!graceful)
        {
            m_tasks.clear();
        }
    }

    std::vector<RecurringId> ids;
    {
        std::lock_guard lock(m_mutex);
        ids.reserve(m_timers.size());
        for (const std::pair<const unsigned long long, TimerRecord> &p : m_timers)
        {
            ids.push_back(p.first);
        }
    }

    for (const size_t id : ids)
    {
        cancelRecurring(id);
    }

    m_cv.notify_all();

    for (std::jthread &w : m_workers)
    {
        w.request_stop();
    }

    m_workers.clear();
}
