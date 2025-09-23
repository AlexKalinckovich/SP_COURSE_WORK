// Logger.cpp
#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <utility>

namespace core::logging
{

static std::string escapeJsonString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);

    for (const UCHAR c : s)
    {
        switch (c)
        {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
            {
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out.push_back(static_cast<char>(c));
                }
            }
        }
    }

    return out;
}

// ---------------------- LogRecord::toNDJsonLine ----------------------
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
    const std::chrono::time_point<std::chrono::system_clock> now = clock::now();
    time_t secs = clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32) || defined(_WIN64)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif

    const std::chrono::duration<LONGLONG, std::ratio<1, 1000>> ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    os << '.' << std::setw(3) << std::setfill('0') << ms.count() << "Z";
    return os.str();
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

    if (m_worker.joinable())
    {
        m_worker.request_stop();
    }

    m_cv.notify_all();


    if (flush)
    {
        flush();
    }
}

void Logger::addSink(std::shared_ptr<ILogSink> sink)
{
    std::lock_guard lk(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::removeSink(const std::shared_ptr<ILogSink> &sink)
{
    std::lock_guard lk(m_mutex);
    std::erase(m_sinks, sink);
}

void Logger::log(LogLevel level,
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
    const int levelInt = static_cast<int>(level);
    int minLevel = 0;
    switch (m_profile)
    {
        case LoggingProfile::Weak:   minLevel = static_cast<int>(LogLevel::Error);   break;
        case LoggingProfile::Medium: minLevel = static_cast<int>(LogLevel::Info);    break;
        case LoggingProfile::Strong: minLevel = static_cast<int>(LogLevel::Trace);   break;
    }

    if (levelInt < minLevel)
    {
        return;
    }

    LogRecord rec;
    rec.timestamp = currentIsoUtcTimestamp();
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
    rec.tid = threadIdToString(std::this_thread::get_id());

    {
        std::unique_lock lk(m_mutex);

        if (m_queue.size() >= m_maxQueue)
        {
            if (m_policy == OverflowPolicy::Block)
            {
                m_cv.wait(lk, [this]() { return m_queue.size() < m_maxQueue; });
                m_queue.emplace_back(std::move(rec));
                m_cv.notify_one();
                return;
            }

            if (m_policy == OverflowPolicy::DropNewest)
            {
                m_droppedCount.fetch_add(1);
                return;
            }

            while (m_queue.size() >= m_maxQueue)
            {
                m_queue.pop_front();
                m_droppedCount.fetch_add(1);
            }
            m_queue.emplace_back(std::move(rec));
            m_cv.notify_one();
            return;
        }

        m_queue.emplace_back(std::move(rec));
    }

    m_cv.notify_one();
}

void Logger::writerLoop(std::stop_token stoken)
{
    constexpr std::chrono::milliseconds kFlushInterval{ 200 };

    while (!stoken.stop_requested())
    {
        constexpr std::size_t kMaxBatch = 128;
        std::vector<LogRecord> batch;
        batch.reserve(kMaxBatch);

        {
            std::unique_lock lk(m_mutex);
            if (m_queue.empty())
            {
                m_cv.wait_for(lk, kFlushInterval, [this, &stoken]()
                {
                    return !m_queue.empty() || stoken.stop_requested();
                });
            }

            while (!m_queue.empty() && batch.size() < kMaxBatch)
            {
                batch.emplace_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }
        }

        if (!batch.empty())
        {
            // dispatch o sinks (catch exceptions to keep writer alive)
            std::lock_guard<std::mutex> lk(m_mutex);
            for (const std::shared_ptr<ILogSink> &sink : m_sinks)
            {
                try
                {
                    sink->consume(batch);
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "Logger sink exception: " << ex.what() << '\n';
                }
                catch (...)
                {
                    std::cerr << "Logger sink unknown exception\n";
                }
            }
        }
    }

    std::vector<LogRecord> finalBatch;
    {
        std::lock_guard lk(m_mutex);
        while (!m_queue.empty())
        {
            finalBatch.emplace_back(std::move(m_queue.front()));
            m_queue.pop_front();
            if (finalBatch.size() >= 128)
            {
                for (const std::shared_ptr<ILogSink> &sink : m_sinks)
                {
                    try { sink->consume(finalBatch); } catch (...) { }
                }
                finalBatch.clear();
            }
        }
    }

    if (!finalBatch.empty())
    {
        for (const std::shared_ptr<ILogSink> &sink : m_sinks)
        {
            try { sink->consume(finalBatch); } catch (...) { }
        }
    }
}

void Logger::flush()
{

    while (true)
    {
        {
            std::lock_guard lk(m_mutex);
            if (m_queue.empty())
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    for (const std::shared_ptr<ILogSink> &sink : m_sinks)
    {
        try
        {
            sink->flush();
        }
        catch (...)
        {}
    }
}

LoggingProfile Logger::profile() const
{
    return m_profile;
}

std::size_t Logger::queueSize() const
{
    std::lock_guard lk(m_mutex);
    return m_queue.size();
}

std::size_t Logger::droppedCount() const
{
    return m_droppedCount.load();
}

} // namespace core::logging
