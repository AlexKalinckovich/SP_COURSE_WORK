// WinThreadPoolAdapter.h
#pragma once

#include "IThreadManager.h"

#include <windows.h>
#include <threadpoolapiset.h>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <atomic>

/**
 * WinThreadPoolAdapter
 *
 * Adapter that implements IThreadManager using the native Windows Thread Pool
 * APIs (work objects and timers).
 *
 * Notes:
 *  - This implementation creates one PTP_WORK per submitted Task. The PTP_WORK
 *    is created with the task context pointer and submitted immediately.
 *    The callback will execute the task and free the context and Close the work object.
 *    This is simple and safe but incurs allocation cost per task.
 *
 *  - scheduleRecurringGeneric uses CreateThreadpoolTimer and SetThreadpoolTimer,
 *    storing the PTP_TIMER in m_timers; cancelRecurring cancels and closes the timer.
 *
 *  - shutdown() cancels and waits for timers and outstanding work callbacks to finish
 *    using WaitForThreadpoolTimerCallbacks / WaitForThreadpoolWorkCallbacks.
 *
 * See implementation file for details and exact cleanup sequence.
 */
class WinThreadPoolAdapter final : public IThreadManager
{
public:
    WinThreadPoolAdapter();

    ~WinThreadPoolAdapter() override;

    WinThreadPoolAdapter(const WinThreadPoolAdapter&) = delete;
    WinThreadPoolAdapter& operator=(const WinThreadPoolAdapter&) = delete;

    void enqueue(Task task) override;

    RecurringId scheduleRecurringGeneric(std::chrono::milliseconds interval, Task task) override;

    void cancelRecurring(RecurringId id) override;

    void shutdown(bool graceful) override;

private:
    struct WorkContext
    {
        Task function;
    };

    struct TimerContext
    {
        Task function;
        LONG periodMs;
        PTP_TIMER timer;
    };

    std::mutex m_mutex;
    std::atomic<RecurringId> m_nextId{ 1 };

    std::unordered_map<RecurringId, std::unique_ptr<TimerContext>> m_timers;

    std::atomic<bool> m_shuttingDown{ false };

    static VOID CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Parameter, PTP_WORK Work);

    static VOID CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE Instance, PVOID Parameter, PTP_TIMER Timer);
};
