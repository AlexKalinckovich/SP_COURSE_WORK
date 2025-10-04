// EventLogSink.cpp
#include "EventLogSink.h"

#include <windows.h>
#include <utility>
#include <vector>
#include <iostream>
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
            return EVENTLOG_INFORMATION_TYPE;
        case LogLevel::Info:
            return EVENTLOG_SUCCESS;
        case LogLevel::Warn:
            return EVENTLOG_WARNING_TYPE;
        case LogLevel::Error:
            return EVENTLOG_ERROR_TYPE;
        case LogLevel::Critical:
            return EVENTLOG_AUDIT_FAILURE;
        default:
            return EVENTLOG_INFORMATION_TYPE;
    }
}

// ---------- constructor / destructor ----------
    EventLogSink::EventLogSink(std::wstring sourceNameW, IThreadManager* threadManager)
        : m_sourceNameW(std::move(sourceNameW))
        , m_hEventLog(nullptr)
        , m_threadManager(threadManager)
        , m_running(false)
        , m_dropped(0ull)
        , m_current_queue_memory(0)
        , m_immediate_flush_requested(false)
{
    m_hEventLog = RegisterEventSourceW(nullptr, m_sourceNameW.c_str());
    if (m_hEventLog == nullptr) {
        const DWORD error = GetLastError();
        std::cerr << "EventLogSink: RegisterEventSourceW failed error=" << error << "\n";
    }

    m_running.store(true, std::memory_order_release);

    if (m_threadManager == nullptr) {
        m_worker = std::jthread([this](const std::stop_token &st) {
            this->writerLoop(st);
        });
    } else {
        startThreadManagerTask();
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

void EventLogSink::consume(const std::vector<LogRecord>& batch)
{
    if (!m_running.load(std::memory_order_acquire))
    {
        m_dropped.fetch_add(batch.size(), std::memory_order_relaxed);
        return;
    }

    std::vector<std::wstring> prepared_batch;
    prepared_batch.reserve(batch.size());

    ULONGLONG total_size = 0;
    for (const LogRecord &record: batch)
    {
        std::string line = record.toNDJsonLine();

        if (line.size() > static_cast<size_t>(kMaxPayloadBytes))
        {
            std::string truncated = line.substr(0, static_cast<size_t>(kMaxPayloadBytes - 128u));
            truncated.append("...[TRUNCATED]");
            if (record.snapshot_id.has_value())
            {
                truncated.append(" snap=");
                truncated.append(record.snapshot_id.value());
            }
            line = std::move(truncated);
        }

        prepared_batch.push_back(utf8ToWide(line));
        total_size += line.size();
    }

    {
        std::lock_guard lk(m_mutex);

        constexpr size_t MAX_QUEUE_SIZE = 10000;
        constexpr ULONGLONG MAX_QUEUE_MEMORY = 100 * 1024 * 1024;

        if (m_queue.size() + prepared_batch.size() > MAX_QUEUE_SIZE ||
            m_current_queue_memory + total_size > MAX_QUEUE_MEMORY)
        {
            while (!m_queue.empty() &&
                   (m_queue.size() + prepared_batch.size() > MAX_QUEUE_SIZE ||
                    m_current_queue_memory + total_size > MAX_QUEUE_MEMORY))
            {
                m_current_queue_memory -= m_queue.front().size() * sizeof(wchar_t);
                m_queue.pop_front();
                m_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }

        for (std::wstring &message: prepared_batch)
        {
            m_queue.push_back(std::move(message));
            m_current_queue_memory += message.size() * sizeof(wchar_t);
        }
    }

    m_cv.notify_one();
}

void EventLogSink::writerLoop(const std::stop_token& stoken)
{
    while (!stoken.stop_requested() || !m_queue.empty())
    {
        bool immediate_flush = false;
        std::vector<std::wstring> batch; {
            std::unique_lock lk(m_mutex);

            if (m_immediate_flush_requested && m_queue.empty())
            {
                m_immediate_flush_requested = false;
                m_cv.notify_all();
                continue;
            }

            if (m_queue.empty())
            {
                m_cv.wait_for(lk, std::chrono::milliseconds(200), [this, &stoken]
                {
                    return !m_queue.empty() || stoken.stop_requested() || m_immediate_flush_requested;
                });
            }

            while (!m_queue.empty() && batch.size() < 64u)
            {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
                m_current_queue_memory -= batch.back().size() * sizeof(wchar_t);
            }

            immediate_flush = m_immediate_flush_requested;
        }


        if (!batch.empty() || immediate_flush)
        {
            processBatch(batch, log_levels);

            if (immediate_flush)
            {
                std::lock_guard lk(m_mutex);
                if (m_queue.empty())
                {
                    m_immediate_flush_requested = false;
                    m_cv.notify_all();
                }
            }
        }
    }
}

// ---------- send single event with ReportEventW ----------
bool EventLogSink::sendEventWide(const std::wstring& wideLine, LogLevel level)
{
    // 1. Проверка и восстановление handle при необходимости
    if (m_hEventLog == nullptr) {
        m_hEventLog = RegisterEventSourceW(nullptr, m_sourceNameW.c_str());
        if (m_hEventLog == nullptr) {
            DWORD error = GetLastError();
            // Критические ошибки (нет прав, источник не зарегистрирован)
            if (error == ERROR_ACCESS_DENIED || error == RPC_S_SERVER_UNAVAILABLE) {
                std::cerr << "EventLogSink: critical error, event logging disabled: " << error << "\n";
                return false;
            }
            // Временные ошибки - попробуем еще раз позже
            return false;
        }
    }

    // 2. Подготовка данных для ReportEventW
    LPCWSTR strings[1] = { wideLine.c_str() };
    const WORD type = mapLevelToEventType(level);

    constexpr DWORD eventId = 1u; // Базовый ID события
    constexpr WORD numStrings = 1u;
    constexpr DWORD dataSize = 0u;
    LPVOID rawData = nullptr;

    // 3. Попытка отправки с повторными попытками при временных ошибках
    const int max_attempts = 3;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        BOOL success = ReportEventW(m_hEventLog, type, 0u, eventId, nullptr,
                                  numStrings, dataSize, strings, rawData);

        if (success) {
            return true;
        }

        DWORD error = GetLastError();

        // Критические ошибки - не повторяем
        if (error == ERROR_ACCESS_DENIED || error == RPC_S_SERVER_UNAVAILABLE) {
            std::cerr << "EventLogSink: critical report error: " << error << "\n";
            break;
        }

        // Временные ошибки - повторяем с задержкой
        if (attempt < max_attempts - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));

            // Попробуем переоткрыть handle при определенных ошибках
            if (error == RPC_S_INVALID_BINDING) {
                if (m_hEventLog != nullptr) {
                    DeregisterEventSource(m_hEventLog);
                    m_hEventLog = nullptr;
                }
                m_hEventLog = RegisterEventSourceW(nullptr, m_sourceNameW.c_str());
            }
        }
    }

    // 4. Все попытки failed - логируем ошибку
    std::cerr << "EventLogSink: failed to report event after " << max_attempts << " attempts\n";
    return false;
}

    void EventLogSink::processBatch(const std::vector<std::wstring>& batch, const std::vector<LogLevel>& levels)
{
    if (batch.empty()) return;

    ULONGLONG successful_sends = 0;
    ULONGLONG failed_sends = 0;

    for (size_t i = 0; i < batch.size(); ++i) {
        const bool success = sendEventWide(batch[i], levels[i]);

        if (success) {
            ++successful_sends;
        } else {
            ++failed_sends;

            if (levels[i] == LogLevel::Error || levels[i] == LogLevel::Critical) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                sendEventWide(batch[i], levels[i]); // Вторая попытка
            }
        }

        // Небольшая задержка между сообщениями чтобы не перегружать Event Log
        if (i < batch.size() - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Обновляем статистику
    if (failed_sends > 0) {
        m_dropped.fetch_add(failed_sends, std::memory_order_relaxed);
    }

    // Логируем статистику для больших batch'ей
    if (batch.size() > 10) {
        std::cout << "EventLogSink: processed batch - " << successful_sends
                  << " successful, " << failed_sends << " failed\n";
    }
}

void EventLogSink::startThreadManagerTask()
{
    // Периодическая задача вместо вечного цикла
    auto task = [this]() -> bool {
        if (!m_running.load(std::memory_order_acquire)) {
            return false; // Завершаем задачу
        }

        std::vector<std::wstring> batch;
        bool immediate_flush = false;

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_queue.empty() && !m_immediate_flush_requested) {
                return true; // Продолжаем, но нет работы
            }

            // Извлекаем batch
            while (!m_queue.empty() && batch.size() < 64u) {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
                m_current_queue_memory -= batch.back().size() * sizeof(wchar_t);
            }

            immediate_flush = m_immediate_flush_requested && m_queue.empty();
        }

        if (!batch.empty() || immediate_flush) {
            processBatch(batch, log_levels);

            if (immediate_flush) {
                std::lock_guard lk(m_mutex);
                if (m_queue.empty()) {
                    m_immediate_flush_requested = false;
                    m_cv.notify_all();
                }
            }
        }

        return true;
    };

    m_threadManager->scheduleRecurring(std::chrono::milliseconds(100), task);
}


// ---------- flush ----------
void EventLogSink::flush()
{ {
        std::lock_guard lk(m_mutex);
        m_immediate_flush_requested = true;
    }
    m_cv.notify_one();

    std::unique_lock lk(m_mutex);
    const auto timeout = std::chrono::seconds(30);

    const bool isSuccessfullyFlushed = m_cv.wait_for(lk, timeout, [this]
    {
        return m_queue.empty() && !m_immediate_flush_requested;
    });
    if (!isSuccessfullyFlushed)
    {
        std::cerr << "EventLogSink: flush timeout after 30 seconds, queue size: "
                << m_queue.size() << "\n";

        m_immediate_flush_requested = false;
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
