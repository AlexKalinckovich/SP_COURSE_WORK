#pragma once

#include <functional>
#include <future>
#include <chrono>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <memory>

/**
 *  IThreadManager
 *
 *  Minimal abstract interface for a thread manager / pool.
 *
 *  Concrete implementations must implement:
 *    - enqueue: push a void() task into the queue / runtime.
 *    - scheduleRecurringGeneric: schedule a void() task to run every interval milliseconds and return an id.
 *    - cancelRecurring: cancel an active recurring task by id.
 *    - shutdown: stop the pool.
 *
 *  The templated helpers `submit` and the templated `scheduleRecurring`
 *  are implemented in terms of the non-template virtual functions.
 *
 *  This design avoids virtual templates while keeping a convenient API.
 */
class IThreadManager
{
public:
    using Task = std::function<void()>;
    using RecurringId = std::size_t;

    virtual ~IThreadManager() = default;

    /**
     * Enqueue a void() task to be executed by the thread manager.
     * This is the only pure-virtual primitive required from implementors.
     */
    virtual void enqueue(Task task) = 0;

    /**
     * Templated submit: wraps callable into a packaged_task, enqueues it and
     * returns a std::future<R>. Implementations must provide enqueue().
     *
     * Usage:
     *   auto fut = mgr->submit([](){ return 42; });
     *   int v = fut.get();
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
    {
        using Result = std::invoke_result_t<F, Args...>;
        using Packaged = std::packaged_task<Result()>;

        Packaged task(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<Result> fut = task.get_future();

        enqueue([t = std::move(task)]() mutable
        {
            t();
        });

        return fut;
    }

    /**
     * Non-template primitive for recurring tasks: concrete classes implement this.
     * The pool returns an id which can be later passed to cancelRecurring(id).
     */
    virtual RecurringId scheduleRecurringGeneric(std::chrono::milliseconds interval, Task task) = 0;

    /**
     * Convenience templated wrapper to schedule arbitrary callables.
     */
    template <typename F, typename... Args>
    RecurringId
    scheduleRecurring(std::chrono::milliseconds interval, F&& f, Args&&... args)
    {
        Task task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        return scheduleRecurringGeneric(interval, std::move(task));
    }

    /**
     * Cancel a recurring task by id.
     */
    virtual void cancelRecurring(RecurringId id) = 0;

    /**
     * Shutdown the thread manager.
     * If graceful == true attempt to drain the queue before exit.
     * If graceful == false stop immediately (may abandon queued tasks).
     */
    virtual void shutdown(bool graceful) = 0;
};
