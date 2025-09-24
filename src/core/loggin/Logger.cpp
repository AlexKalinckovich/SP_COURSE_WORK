// Logger.cpp
#include "Logger.h"

#include <chrono>
#include <ctime>
#include <sstream>
#include <iostream>
#include <utility>

namespace core::logging
{

/**
 * Escapes a string for safe inclusion in JSON by replacing control characters
 * and special characters with their JSON escape sequences.
 *
 * This function handles:
 * - Standard JSON escapes: quotation marks, backslashes, and common control characters
 * - Unicode escapes for other control characters (U+0000 to U+001F)
 * - Optimizes for the common case where no escaping is needed
 *
 * @param input The string view to be escaped
 * @return A properly escaped JSON string
 */
static std::string escapeJsonString(const std::string_view input)
{
    static constexpr unsigned char MAX_CONTROL_CHAR = 0x20;
    static constexpr unsigned char HEX_DIGIT_MASK = 0x0F;
    static constexpr size_t EXTRA_ESCAPE_CHARS = 5;
    static constexpr size_t RESERVE_SAFETY_MARGIN = 16;

    std::size_t escape_count = 0;
    for (const unsigned char c: input)
    {
        if (c < MAX_CONTROL_CHAR || c == '\\' || c == '\"')
        {
            escape_count++;
        }
    }

    if (escape_count == 0)
    {
        return std::string(input);
    }

    std::string result;
    result.reserve(input.size() + escape_count * EXTRA_ESCAPE_CHARS + RESERVE_SAFETY_MARGIN);

    for (const signed char c: input)
    {
        switch (c)
        {
            case '\"': result += "\\\"";
                break;
            case '\\': result += "\\\\";
                break;
            case '\b': result += "\\b";
                break;
            case '\f': result += "\\f";
                break;
            case '\n': result += "\\n";
                break;
            case '\r': result += "\\r";
                break;
            case '\t': result += "\\t";
                break;
            default:
                if (c < MAX_CONTROL_CHAR)
                {
                    constexpr char hexdigits[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hexdigits[(c >> 4) & HEX_DIGIT_MASK];
                    result += hexdigits[c & HEX_DIGIT_MASK];
                }
                else
                {
                    result += c;
                }
        }
    }

    return result;
}

// ---------------------- LogRecord::toNDJsonLine ----------------------
/**
 * Converts a LogRecord to Newline Delimited JSON (NDJSON) format.
 *
 * NDJSON format has one JSON object per line, making it suitable for log files.
 * Only non-empty fields are included in the output.
 *
 * @return A string containing the JSON representation of the log record followed by newline
 */
std::string LogRecord::toNDJsonLine() const
{
    std::ostringstream os;
    os << "{";

    os << R"("@ts":")" << escapeJsonString(timestamp) << "\"";
    os << R"(,"lvl":")" << static_cast<int>(level) << "\"";

    if (!message.empty())
    {
        os << R"(,"msg":")" << escapeJsonString(message) << "\"";
    }
    if (!operation.empty())
    {
        os << R"(,"op":")" << escapeJsonString(operation) << "\"";
    }
    if (!key_path.empty())
    {
        os << R"(,"key":")" << escapeJsonString(key_path) << "\"";
    }
    if (!value_name.empty())
    {
        os << R"(,"val":")" << escapeJsonString(value_name) << "\"";
    }
    if (!before.empty())
    {
        os << R"(,"before":")" << escapeJsonString(before) << "\"";
    }
    if (!after.empty())
    {
        os << R"(,"after":")" << escapeJsonString(after) << "\"";
    }
    if (snapshot_id.has_value())
    {
        os << R"(,"snap":")" << escapeJsonString(snapshot_id.value()) << "\"";
    }
    if (metadata.has_value())
    {
        os << R"(,"meta":")" << escapeJsonString(metadata.value()) << "\"";
    }
    if (!source.empty())
    {
        os << R"(,"src":")" << escapeJsonString(source) << "\"";
    }

    os << ",\"pid\":" << pid;
    os << R"(,"tid":")" << escapeJsonString(tid) << "\"";

    os << "}\n";
    return os.str();
}

// ---------------------- helpers ----------------------
std::string Logger::currentIsoUtcTimestamp()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    time_t secs = clock::to_time_t(now);
    std::tm tm{};

#ifdef _WIN32
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 static_cast<int>(ms.count()));

    return {buffer};
}

std::string Logger::threadIdToString(std::thread::id id)
{
    std::ostringstream os;
    os << id;
    return os.str();
}

// ---------------------- Logger implementation ----------------------
Logger::Logger(const std::size_t maxQueue, const LoggingProfile profile, const OverflowPolicy policy)
    : m_maxQueue(maxQueue)
    , m_profile(profile)
    , m_policy(policy)
{
}

Logger::~Logger()
{
    try
    {
        shutdown(true);
    }
    catch (...)
    {}
}

void Logger::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true))
    {
        return;
    }

    m_worker = std::jthread([this](std::stop_token st) -> void
    {
        this->writerLoop(std::move(st));
    });
}

void Logger::shutdown(bool flush)
{
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false))
    {
        return;
    }

    if (flush)
    {
        log(LogLevel::Info, "Logger shutdown", "shutdown");
    }

    if (m_worker.joinable())
    {
        m_worker.request_stop();
        m_cv.notify_all();

        if (m_worker.joinable())
        {
            const std::future<void> future = std::async(std::launch::async, [this]
            {
                m_worker.join();
            });

            if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            {
                std::cerr << "Logger shutdown timeout - forcing termination\n";
                m_worker.detach();
            }
        }
    }

    if (flush)
    {
        flushAllSinks();
    }
}

void Logger::addSink(std::shared_ptr<ILogSink> sink)
{
    if (!sink) {
        throw std::invalid_argument("Cannot add null sink");
    }

    std::lock_guard lk(m_mutex);

    const auto it = std::ranges::find(m_sinks, sink);
    if (it == m_sinks.end()) {
        m_sinks.push_back(std::move(sink));

        m_cv.notify_one();
    }
}


void Logger::removeSink(const std::shared_ptr<ILogSink>& sink)
{
    if (!sink) return;

    std::unique_lock lk(m_mutex);

    const auto it = std::ranges::find(m_sinks, sink);
    if (it != m_sinks.end()) {
        m_cv.wait(lk, [this] {
            return active_batches.load(std::memory_order_acquire) == 0;
        });

        m_sinks.erase(it);
    }
}

template<typename Predicate>
void Logger::removeSinkIf(Predicate pred)
{
    std::unique_lock lk(m_mutex);

    m_cv.wait(lk, [this] {
        return active_batches.load(std::memory_order_acquire) == 0;
    });

    m_sinks.erase(std::remove_if(m_sinks.begin(), m_sinks.end(), pred), m_sinks.end());
}

void Logger::log(const LogLevel level,
                 std::string message,
                 std::string operation,
                 std::string key_path,
                 std::string value_name,
                 std::string before,
                 std::string after,
                 std::string source,
                 std::optional<std::string> snapshot_id,
                 std::optional<std::string> metadata)
{
    if (shouldSkipByProfile(level))
    {
        return;
    }

    thread_local std::string cached_tid = threadIdToString(std::this_thread::get_id());
    thread_local std::chrono::system_clock::time_point last_time_update;
    thread_local std::string cached_timestamp;

    const auto now = std::chrono::system_clock::now();
    if (cached_timestamp.empty() ||
        now - last_time_update > std::chrono::seconds(1))
    {
        cached_timestamp = currentIsoUtcTimestamp();
        last_time_update = now;
    }

    LogRecord rec;
    rec.timestamp = cached_timestamp;
    rec.level = level;
    rec.message = std::move(message);
    rec.operation = std::move(operation);
    rec.key_path = std::move(key_path);
    rec.value_name = std::move(value_name);
    rec.before = std::move(before);
    rec.after = std::move(after);
    rec.source = std::move(source);
    rec.snapshot_id = std::move(snapshot_id);
    rec.metadata = std::move(metadata);
    rec.pid = ::GetCurrentProcessId();
    rec.tid = cached_tid; {
        std::unique_lock lk(m_mutex);

        if (m_queue.size() >= m_maxQueue)
        {
            handleOverflowPolicy(lk, rec);
            return;
        }

        m_queue.emplace_back(std::move(rec));
        m_atomicQueueSize.store(m_queue.size(), std::memory_order_relaxed);
    }

    m_cv.notify_one();
}

bool Logger::shouldSkipByProfile(LogLevel level) const
{
    const int levelInt = static_cast<int>(level);
    int minLevel = 0;
    switch (m_profile)
    {
        case LoggingProfile::Weak:   minLevel = static_cast<int>(LogLevel::Error);   break;
        case LoggingProfile::Medium: minLevel = static_cast<int>(LogLevel::Info);    break;
        case LoggingProfile::Strong: minLevel = static_cast<int>(LogLevel::Trace);   break;
    }
    return levelInt < minLevel;
}

void Logger::handleOverflowPolicy(std::unique_lock<std::mutex>& lk, LogRecord& rec)
{
    switch (m_policy)
    {
        case OverflowPolicy::Block:
            m_cv.wait(lk, [this] { return m_queue.size() < m_maxQueue; });
            m_queue.emplace_back(std::move(rec));
            m_cv.notify_one();
            break;

        case OverflowPolicy::DropNewest:
            m_droppedCount.fetch_add(1, std::memory_order_relaxed);
            break;

        case OverflowPolicy::DropOldest:
            while (m_queue.size() >= m_maxQueue)
            {
                m_queue.pop_front();
                m_droppedCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_queue.emplace_back(std::move(rec));
            m_cv.notify_one();
            break;
    }
}

void Logger::writerLoop(std::stop_token stoken)
{
    constexpr std::chrono::milliseconds kFlushInterval{200};

    while (!stoken.stop_requested())
    {
        std::vector<LogRecord> batch;
        std::vector<std::shared_ptr<ILogSink> > sinks_copy;

        {
            constexpr std::size_t kMaxBatch = 128;
            std::unique_lock lk(m_mutex);

            if (m_queue.empty())
            {
                m_cv.wait_for(lk, kFlushInterval, [this, &stoken]()
                {
                    return !m_queue.empty() || stoken.stop_requested();
                });
            }

            batch.reserve(std::min(kMaxBatch, m_queue.size()));
            while (!m_queue.empty() && batch.size() < kMaxBatch)
            {
                batch.emplace_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }
            m_atomicQueueSize.store(m_queue.size(), std::memory_order_relaxed);
            sinks_copy = m_sinks;
        }

        if (!batch.empty() && !sinks_copy.empty())
        {
            {
                active_batches.fetch_add(1, std::memory_order_relaxed);
            }
            processBatchWithSinks(batch, sinks_copy);
            {
                active_batches.fetch_sub(1, std::memory_order_relaxed);
                m_cv.notify_all();
            }
        }

        if (batch.empty())
        {
            std::this_thread::yield();
        }
    }

    processRemainingRecords(stoken);
}

void Logger::processBatchWithSinks(const std::vector<LogRecord>& batch,
                                   const std::vector<std::shared_ptr<ILogSink>>& sinks)
{
    for (const std::shared_ptr<ILogSink> &sink: sinks)
    {
        try
        {
            sink->consume(batch);
        } catch (const std::exception &ex)
        {
            std::cerr << "Logger sink exception: " << ex.what() << '\n';
        }
        catch (...)
        {
            std::cerr << "Logger sink unknown exception\n";
        }
    }
}

void Logger::processRemainingRecords(const std::stop_token &stoken)
{
    constexpr std::size_t kFinalBatchSize = 128;
    std::vector<LogRecord> final_batch;
    final_batch.reserve(kFinalBatchSize);

    while (!stoken.stop_requested())
    {
        std::vector<std::shared_ptr<ILogSink> > sinks_copy;
        {
            std::lock_guard lk(m_mutex);
            if (m_queue.empty()) break;

            while (!m_queue.empty() && final_batch.size() < kFinalBatchSize)
            {
                final_batch.emplace_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }
            sinks_copy = m_sinks;
        }

        if (!final_batch.empty())
        {
            processBatchWithSinks(final_batch, sinks_copy);
            final_batch.clear();
        }
    }
}

void Logger::flush()
{
    {
        std::unique_lock lk(m_mutex);
        m_cv.wait(lk, [this] {
            return m_queue.empty() && active_batches.load() == 0;
        });
    }

    flushAllSinks();
}

void Logger::flushAllSinks() const
{
    std::vector<std::shared_ptr<ILogSink>> sinks_copy;
    {
        std::lock_guard lk(m_mutex);
        sinks_copy = m_sinks;
    }

    for (const std::shared_ptr<ILogSink>& sink : sinks_copy) {
        try {
            sink->flush();
        }
        catch (const std::exception& ex) {
            std::cerr << "Flush failed: " << ex.what() << '\n';
        }
        catch (...) {
            std::cerr << "Flush failed with unknown exception\n";
        }
    }
}

LoggingProfile Logger::profile() const noexcept
{
    return m_profile;
}

std::size_t Logger::queueSize() const noexcept
{
    return m_atomicQueueSize.load(std::memory_order_relaxed);
}

std::size_t Logger::droppedCount() const noexcept
{
    return m_droppedCount.load(std::memory_order_relaxed);
}

LoggerStats Logger::getStats() const noexcept
{
    return LoggerStats{
        .queue_size = m_atomicQueueSize.load(std::memory_order_relaxed),
        .dropped_count = m_droppedCount.load(std::memory_order_relaxed),
        .active_batches = active_batches.load(std::memory_order_relaxed),
        .is_running = m_running.load(std::memory_order_relaxed)
    };
}

} // namespace core::logging
