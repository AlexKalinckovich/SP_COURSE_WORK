// Logger.h
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "WinThreadPoolAdapter.h"

#define MAX_LOGGING_QUEUE_SIZE (64 * 1024)
/**
 * Core asynchronous logging subsystem.
 *
 * Design:
 *  - Produce structured LogRecord objects (serializable to NDJSON).
 *  - Logger accepts LogRecord via non-blocking enqueue; a background writer thread
 *    drains the queue and dispatches batches to registered sinks (ILogSink).
 *  - Sinks implement durable storage (FileLogger, EventLogSink, other).
 *
 * Important: Logger is designed to never block UI threads by default. The default
 * overflow policy is to drop oldest messages when the queue is full (keeping recent).
 */

namespace core::logging
{

enum class LogLevel
{
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

enum class LoggingProfile
{
    Weak,
    Medium,
    Strong
};

enum class OverflowPolicy
{
    DropOldest,
    DropNewest,
    Block
};

/**
 * LogRecord - single structured log entry.
 *
 * Fields:
 *  - timestamp: ISO8601 UTC timestamp (generated at log() time)
 *  - level: severity
 *  - message: human message
 *  - operation: short op name (e.g. "set_value", "snapshot_create")
 *  - key_path, value_name: registry-specific fields (may be empty)
 *  - before/after: optional snapshots or small diffs (strings containing JSON)
 *  - source: "ui" or "external"
 *  - metadata: free-form JSON string (if available)
 *  - pid/tid: process and thread info (filled automatically)
 *
 * The LogRecord provides toNDJsonLine() to serialize as a single NDJSON line.
 */
struct LogRecord
{
    std::string timestamp;
    LogLevel level = LogLevel::Info;
    std::string message;
    std::string operation;
    std::string key_path;
    std::string value_name;
    std::string before;
    std::string after;
    std::string source;
    std::optional<std::string> snapshot_id;
    std::optional<std::string> metadata;
    ULONG pid = 0;
    std::string tid;
    [[nodiscard]] std::string toNDJsonLine() const;
};

/**
 * ILogSink - sink interface. Implement this for FileLogger, EventLogSink etc.
 *
 * Contract:
 *  - `consume` is called by the writer thread with a batch of LogRecord objects.
 *  - Implementations must be exception-safe: throw nothing (the logger will catch).
 *  - flush() should block until all prior consume() calls are durable.
 */
class ILogSink
{
public:
    virtual ~ILogSink() = default;

    virtual void
    consume(const std::vector<LogRecord>& batch) = 0;

    virtual void
    flush() = 0;
};

/**
 * Logger - central manager.
 *
 * Usage:
 *   Logger logger;
 *   logger.addSink(std::make_shared<FileLogger>("app.log"));
 *   logger.start();
 *   logger.log(LogLevel::Info, "app started", "startup", ...);
 *   logger.shutdown(true);
 *
 * Thread-safety:
 *  - log() and addSink()/removeSink() are thread-safe.
 */
class Logger
{
public:
    explicit Logger(std::size_t maxQueue = MAX_LOGGING_QUEUE_SIZE,
                    LoggingProfile profile = LoggingProfile::Medium,
                    OverflowPolicy policy = OverflowPolicy::DropOldest);

    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void start();

    void shutdown(bool flush = true);

    void addSink(std::shared_ptr<ILogSink> sink);

    void removeSink(const std::shared_ptr<ILogSink> &sink);

    void log(LogLevel level,
        std::string message,
        std::string operation = {},
        std::string key_path = {},
        std::string value_name = {},
        std::string before = {},
        std::string after = {},
        std::string source = "ui",
        std::optional<std::string> snapshot_id = std::nullopt,
        std::optional<std::string> metadata = std::nullopt);

    void flush();

    LoggingProfile profile() const;

    std::size_t queueSize() const;

    std::size_t droppedCount() const;

private:
    void writerLoop(std::stop_token stoken);

    static std::string currentIsoUtcTimestamp();

    static std::string threadIdToString(std::thread::id id);

    std::size_t m_maxQueue;
    LoggingProfile m_profile;
    OverflowPolicy m_policy;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<LogRecord> m_queue;
    std::vector<std::shared_ptr<ILogSink>> m_sinks;

    std::jthread m_worker;
    std::atomic<bool> m_running{ false };
    std::atomic<std::size_t> m_droppedCount{ 0 };
};
} // namespace core::logging
