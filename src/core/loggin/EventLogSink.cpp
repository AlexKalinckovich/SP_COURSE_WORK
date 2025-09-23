// EventLogSink.cpp
#include "EventLogSink.h"

#include <windows.h>
#include <utility>
#include <utility>
#include <vector>
#include <iostream>

// Documentation references:
//
// RegisterEventSource / DeregisterEventSource: get/close handle to event log.
//  https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-registereventsourcea
//  https://learn.microsoft.com/en-us/windows/win32/eventlog/event-logging-functions
//  (If the source is missing the API will use Application log; installer should create sources).
//  See: RegisterEventSource remarks. :contentReference[oaicite:9]{index=9}
//
// ReportEvent: used to write the event entry. Size limitations exist; be cautious with large payloads.
//  https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reporteventa
//  On Vista+ there are practical limits ~61,440 bytes for message payloads in RPC transfer; large blobs should not be sent. :contentReference[oaicite:10]{index=10}
//
// Best practice: register event sources at install time (requires admin); do not attempt to create message DLLs at runtime.
//  See Reporting Events guidance. :contentReference[oaicite:11]{index=11}
//
// ETW is an alternative for high-throughput telemetry (EventWrite/EventRegister). Use EventLog for administrative events. :contentReference[oaicite:12]{index=12}

namespace core::logging
{

unsigned long const EventLogSink::kMaxPayloadBytes = 61440u; // ~60 KiB conservative

// ---------- helpers ----------
std::wstring EventLogSink::utf8ToWide(std::string const& utf8)
{
    if (utf8.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.length()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring result;
    result.resize(static_cast<std::size_t>(required));
    int converted = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.length()),
                                        &result[0], required);
    if (converted <= 0)
    {
        return {};
    }

    return result;
}

WORD EventLogSink::mapLevelToEventType(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Trace:
        case LogLevel::Debug:
        case LogLevel::Info:
        {
            return EVENTLOG_INFORMATION_TYPE;
        }
        case LogLevel::Warn:
        {
            return EVENTLOG_WARNING_TYPE;
        }
        case LogLevel::Error:
        case LogLevel::Critical:
        {
            return EVENTLOG_ERROR_TYPE;
        }
        default:
        {
            return EVENTLOG_INFORMATION_TYPE;
        }
    }
}

// ---------- constructor / destructor ----------
EventLogSink::EventLogSink(std::wstring  sourceNameW, IThreadManager* threadManager)
    : m_sourceNameW(std::move(sourceNameW))
    , m_hEventLog(nullptr)
    , m_threadManager(threadManager)
    , m_running(false)
    , m_dropped(0ull)
{
    m_hEventLog = RegisterEventSourceW(nullptr, m_sourceNameW.c_str());
    if (m_hEventLog == nullptr)
    {
        const DWORD err = GetLastError();
        std::cerr << "EventLogSink: RegisterEventSourceW failed error=" << err << "\n";
        // Proceed with m_hEventLog == nullptr: ReportEvent will fail later; keep sink alive so other sinks can work.
    }

    m_running.store(true);

    if (m_threadManager == nullptr)
    {
        m_worker = std::jthread([this](const std::stop_token &st)
        {
            this->writerLoop(st);
        });
    }
    else
    {
        const IThreadManager::Task task = [this]()
        {
            while (m_running.load())
            {
                {
                    std::unique_lock<std::mutex> lk(m_mutex);
                    if (m_queue.empty())
                    {
                        lk.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue;
                    }
                }

                std::vector<std::wstring> batch;
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    while (!m_queue.empty())
                    {
                        batch.push_back(std::move(m_queue.front()));
                        m_queue.pop_front();
                        if (batch.size() >= 64u)
                        {
                            break;
                        }
                    }
                }

                for (const auto & i : batch)
                {
                    this->sendEventWide(i, LogLevel::Info);
                }
            }
        };

        m_threadManager->enqueue(task);
    }
}

EventLogSink::~EventLogSink()
{
    try
    {
        close();
    }
    catch (...)
    {
        // must not throw
    }
}

void EventLogSink::consume(std::vector<LogRecord> const& batch)
{
    if (!m_running.load())
    {
        m_dropped.fetch_add(1ull);
        return;
    }

    std::lock_guard<std::mutex> lk(m_mutex);

    for (const auto & i : batch)
    {
        std::string line = i.toNDJsonLine();

        std::size_t bytes = line.size();
        if (bytes > static_cast<std::size_t>(kMaxPayloadBytes))
        {
            std::string truncated = line.substr(0, static_cast<std::size_t>(kMaxPayloadBytes - 128u));
            truncated.append("...[TRUNCATED]");
            if (i.snapshot_id.has_value())
            {
                truncated.append(" snap=");
                truncated.append(i.snapshot_id.value());
            }
            m_queue.push_back(utf8ToWide(truncated));
        }
        else
        {
            std::wstring wide = utf8ToWide(line);
            m_queue.push_back(std::move(wide));
        }
    }

    m_cv.notify_one();
}

void EventLogSink::writerLoop(const std::stop_token &stoken)
{
    while (!stoken.stop_requested() || !m_queue.empty())
    {
        std::vector<std::wstring> batch;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            if (m_queue.empty())
            {
                m_cv.wait_for(lk, std::chrono::milliseconds(200));
            }

            while (!m_queue.empty() && batch.size() < 64u)
            {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }
        }

        for (const auto & i : batch)
        {
            const LogLevel level = LogLevel::Info;
            const bool ok = sendEventWide(i, level);
            if (!ok)
            {
                m_dropped.fetch_add(1ull);
            }
        }
    }

    // flush finished; deregistration handled in close()
}

// ---------- send single event with ReportEventW ----------
bool EventLogSink::sendEventWide(std::wstring const& wideLine, const LogLevel level)
{
    if (m_hEventLog == nullptr)
    {
        m_hEventLog = RegisterEventSourceW(nullptr, m_sourceNameW.c_str());
        if (m_hEventLog == nullptr)
        {
            const DWORD err = GetLastError();
            std::cerr << "EventLogSink: RegisterEventSourceW retry failed error=" << err << "\n";
            return false;
        }
    }

    LPCWSTR strings[1];
    strings[0] = wideLine.c_str();

    const WORD type = mapLevelToEventType(level);

    constexpr DWORD eventId = 0u;
    constexpr WORD numStrings = 1u;
    constexpr DWORD dataSize = 0u;

    LPVOID rawData = nullptr;

    const BOOL res = ReportEventW(m_hEventLog,
                                  type,
                                  0u,
                                  eventId,
                                  nullptr,
                                  numStrings,
                                  dataSize,
                                  strings,
                                  rawData);

    if (res == FALSE)
    {
        const DWORD err = GetLastError();
        std::cerr << "EventLogSink: ReportEventW failed error=" << err << "\n";
        return false;
    }

    return true;
}

// ---------- flush ----------
void
EventLogSink::flush()
{
    // Wait until queue empty
    while (true)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_queue.empty())
            {
                break;
            }
        }
        m_cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ---------- close ----------
void
EventLogSink::close()
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false))
    {
    }

    m_cv.notify_all();

    if (m_worker.joinable())
    {
        m_worker.request_stop();
    }

    flush();

    if (m_hEventLog != nullptr)
    {
        DeregisterEventSource(m_hEventLog);
        m_hEventLog = nullptr;
    }
}

} // namespace core::logging
