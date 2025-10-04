// EventLogSink.h
#pragma once

#include "Logger.h" // LogRecord, ILogSink
#include "IThreadManager.h"

#include <windows.h>
#include <string>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <sstream>

/**
 * EventLogSink
 *
 * Writes LogRecord entries into the Windows Event Log using ReportEvent.
 *
 * Usage:
 *   // default: internal writer thread
 *   std::shared_ptr<EventLogSink> sink = std::make_shared<EventLogSink>(L"MyAppSource");
 *   logger.addSink(sink);
 *
 *   // or provide a thread manager (non-owning). If provided, sink will use it
 *   // to submit a long-running writer task (this occupies a pool thread).
 *   EventLogSink sinkWithPool(L"MyAppSource", threadManagerPointer);
 *
 * Notes:
 *  - Event source registration should be done at install time. Creating a new
 *    source at runtime requires admin rights and proper message DLLs.
 *  - ReportEvent has payload size limits; the sink truncates overlong messages
 *    and writes a shorter entry pointing to snapshot ids instead.
 *
 * See documentation references in the implementation file.
 */
namespace core::logging
{

class EventLogSink final : public ILogSink
{
public:
    /**
     * Constructor.
     *
     * Parameters:
     *   sourceNameW - UTF-16 event source name (e.g. L"MyCompany\\MyApp" or L"MyAppSource")
     *   threadManager - optional non-owning pointer to IThreadManager; if not null,
     *                   the sink will submit a long-running writer task to it.
     */
    explicit EventLogSink(std::wstring  sourceNameW, IThreadManager* threadManager = nullptr);

    ~EventLogSink() override;

    void consume(std::vector<LogRecord> const& batch) override;

    void flush() override;

    void close();

private:

    void writerLoop(const std::stop_token &stoken);

    bool sendEventWide(std::wstring const& wideLine, LogLevel level);
    void processBatch(const std::vector<std::wstring>& batch, const std::vector<LogLevel>& levels);
    static std::wstring utf8ToWide(std::string const& utf8);

    static WORD mapLevelToEventType(LogLevel level);
    void startThreadManagerTask();

    std::wstring m_sourceNameW;
    HANDLE m_hEventLog;

    std::deque<std::wstring> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::jthread m_worker;
    IThreadManager* m_threadManager;
    std::atomic<bool> m_running;

    std::atomic<unsigned long long> m_dropped;
    ULONGLONG m_current_queue_memory{0};
    std::atomic<bool> m_immediate_flush_requested{false};
    static unsigned long const kMaxPayloadBytes;
    std::vector<LogLevel> log_levels = std::vector{LogLevel::Debug};
};

} // namespace core::logging
