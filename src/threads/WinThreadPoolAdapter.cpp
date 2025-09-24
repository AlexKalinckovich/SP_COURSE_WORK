//
// Created by brota on 23.09.2025.
//
// WinThreadPoolAdapter.cpp
#include "WinThreadPoolAdapter.h"

#include <stdexcept>
#include <cassert>

WinThreadPoolAdapter::WinThreadPoolAdapter() = default;

WinThreadPoolAdapter::~WinThreadPoolAdapter()
{
    try
    {
        shutdown(true);
    }
    catch (...)
    {}
}

/**
 * enqueue
 *
 * Create a PTP_WORK object with a heap-allocated WorkContext containing the std::function.
 * Submit the work object to the default process thread pool using SubmitThreadpoolWork.
 *
 * The WorkCallback will:
 *  - cast the Parameter back to WorkContext*
 *  - invoke the stored function()
 *  - call WaitForThreadpoolWorkCallbacks(Work, FALSE) if needed (not required here)
 *  - CloseThreadpoolWork(Work)
 *  - delete the WorkContext
 *
 * This pattern gives per-call context and ensures safe cleanup after callback finishes.
 */
void WinThreadPoolAdapter::enqueue(Task task)
{
    if (m_shuttingDown.load())
    {
        throw std::runtime_error("WinThreadPoolAdapter is shutting down");
    }

    const auto ctx = new WorkContext{ std::move(task) };

    PTP_WORK work = CreateThreadpoolWork(&WinThreadPoolAdapter::WorkCallback, ctx, nullptr);

    if (work == nullptr)
    {
        delete ctx;
        throw std::runtime_error("CreateThreadpoolWork failed");
    }

    SubmitThreadpoolWork(work);
}

/**
 * static WorkCallback
 *
 * Called by the Windows thread pool on a worker thread when SubmitThreadpoolWork is executed.
 *
 * Parameters:
 *  - Instance: callback instance (unused here)
 *  - Parameter: pointer passed at CreateThreadpoolWork time (WorkContext*)
 *  - Work: the PTP_WORK representing this work item
 *
 * Implementation:
 *  - cast Parameter -> WorkContext*
 *  - invoke function()
 *  - call CloseThreadpoolWork(Work) to release the work object. CloseThreadpoolWork
 *    will free it immediately if no callbacks outstanding or asynchronously otherwise.
 *  - delete the WorkContext
 */
VOID CALLBACK WinThreadPoolAdapter::WorkCallback(PTP_CALLBACK_INSTANCE /*Instance*/, PVOID Parameter, PTP_WORK Work)
{
    const WorkContext* ctx = static_cast<WorkContext*>(Parameter);
    if (ctx == nullptr)
    {
        return;
    }

    try
    {
        ctx->function();
    }
    catch (...)
    {}

    CloseThreadpoolWork(Work);
    delete ctx;
}

/**
 * scheduleRecurringGeneric
 *
 * Create a PTP_TIMER and a TimerContext that will be stored in m_timers.
 * The TimerCallback will be called by the thread pool when the timer fires.
 * The callback will submit a PTP_WORK that runs the user Task (same pattern as enqueue).
 *
 * We store the TimerContext so we can cancel and close the timer later on cancelRecurring or shutdown.
 */
IThreadManager::RecurringId WinThreadPoolAdapter::scheduleRecurringGeneric(std::chrono::milliseconds interval, Task task)
{
    if (m_shuttingDown.load())
    {
        throw std::runtime_error("WinThreadPoolAdapter is shutting down");
    }

    RecurringId id = m_nextId.fetch_add(1);

    std::unique_ptr<TimerContext> ctx = std::make_unique<TimerContext>();
    ctx->function = std::move(task);
    ctx->periodMs = static_cast<LONG>(interval.count());
    ctx->timer = CreateThreadpoolTimer(&WinThreadPoolAdapter::TimerCallback, ctx.get(), nullptr);
    if (ctx->timer == nullptr)
    {
        throw std::runtime_error("CreateThreadpoolTimer failed");
    }

    FILETIME ft = { 0 };
    const LONGLONG due100ns = -static_cast<LONGLONG>(ctx->periodMs) * 10000LL;
    const auto due = static_cast<ULONGLONG>(due100ns);
    ft.dwLowDateTime = static_cast<DWORD>(due & 0xffffffffULL);
    ft.dwHighDateTime = static_cast<DWORD>(due >> 32);

    SetThreadpoolTimer(ctx->timer, &ft, ctx->periodMs, 0);
    {
        std::lock_guard lock(m_mutex);
        m_timers.emplace(id, std::move(ctx));
    }

    return id;
}

/**
 * TimerCallback
 *
 * The callback receives the TimerContext in Parameter. We need to resubmit the user's
 * Task to the thread pool for execution. To reuse the WorkCallback machinery, we allocate
 * a WorkContext and create+submit a PTP_WORK with the WorkContext as Parameter.
 *
 * Important: Do not free the TimerContext here. It remains owned by m_timers until cancelRecurring.
 */
VOID CALLBACK WinThreadPoolAdapter::TimerCallback(PTP_CALLBACK_INSTANCE /*Instance*/, PVOID Parameter, PTP_TIMER /*Timer*/)
{
    const auto tctx = static_cast<TimerContext*>(Parameter);
    if (tctx == nullptr)
    {
        return;
    }

    const auto wctx = new WorkContext{ tctx->function };

    PTP_WORK work = CreateThreadpoolWork(&WinThreadPoolAdapter::WorkCallback, wctx, nullptr);
    if (work == nullptr)
    {
        delete wctx;
        return;
    }

    SubmitThreadpoolWork(work);
}

/**
 * cancelRecurring
 *
 * Cancel and close the timer associated with id. This calls SetThreadpoolTimer with NULL
 * to cancel, waits for outstanding callbacks (WaitForThreadpoolTimerCallbacks) and then
 * CloseThreadpoolTimer. Finally, remove the TimerContext from the map and let it be freed.
 */
void WinThreadPoolAdapter::cancelRecurring(RecurringId id)
{
    std::unique_ptr<TimerContext> ctx;

    {
        std::lock_guard lock(m_mutex);
        auto it = m_timers.find(id);
        if (it == m_timers.end())
        {
            return;
        }
        ctx = std::move(it->second);
        m_timers.erase(it);
    }

    SetThreadpoolTimer(ctx->timer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(ctx->timer, TRUE); // TRUE cancels pending callbacks

    CloseThreadpoolTimer(ctx->timer);
}

/**
 * shutdown
 *
 * Cancel timers and wait for outstanding work callbacks. If graceful == false we
 * attempt to cancel callbacks pending.
 *
 * Steps:
 *  - flip m_shuttingDown
 *  - snapshot current timers and cancel them (SetThreadpoolTimer(NULL...))
 *  - WaitForThreadpoolTimerCallbacks + CloseThreadpoolTimer for each
 *  - There is no global list of PTP_WORK objects created by clients here; each created work
 *    object is closed by its callback, so after the above waits outstanding work callbacks
 *    should complete. If you need to track every work object created externally, keep a list.
 */
void WinThreadPoolAdapter::shutdown(bool /*graceful*/)
{
    bool expected = false;
    if (!m_shuttingDown.compare_exchange_strong(expected, true))
    {
        return;
    }

    std::vector<std::unique_ptr<TimerContext>> timers;
    {
        std::lock_guard lock(m_mutex);
        for (std::pair<const ULONGLONG, std::unique_ptr<TimerContext>> &p : m_timers)
        {
            timers.push_back(std::move(p.second));
        }
        m_timers.clear();
    }

    for (const std::unique_ptr<TimerContext> &ctx : timers)
    {
        SetThreadpoolTimer(ctx->timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(ctx->timer, TRUE);
        CloseThreadpoolTimer(ctx->timer);
    }
}
