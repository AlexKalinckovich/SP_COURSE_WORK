#pragma once

#include "Logger.h" // for LogRecord and ILogSink
#include <windows.h> // WinAPI types and functions
#include <string>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

/**
 * FileLogger
 *
 * Implementation of ILogSink that writes NDJSON lines to a file on disk using WinAPI.
 *
 * Behavior summary:
 *  - consume(batch) enqueues NDJSON lines quickly (non-blocking unless internal queue full).
 *  - A dedicated writer thread flushes queued lines to disk periodically or when buffer threshold is reached.
 *  - Supports file rotation by size, optional FlushFileBuffers calls for durability,
 *    and graceful shutdown via flush().
 *
 * Thread-safety:
 *  - consume(), flush(), and destructor are thread-safe.
 *
 * Constructor parameters explained:
 *  - logDirectoryW: wide string path to directory where logs will be stored (created if missing).
 *  - baseFileName: base filename (e.g., L"registry.log"), rotated files will be suffixed.
 *  - maxFileBytes: rotate when current file reaches this size.
 *  - rotateCount: number of rotated files to keep.
 *  - flushIntervalMs: maximum milliseconds between disk flushes.
 *  - flushThresholdBytes: flush when buffer reaches this many bytes.
 *  - fsyncOnFlush: if true call FlushFileBuffers after WriteFile.
 */
namespace core::logging
{

class FileLogger final : public ILogSink
{
public:
    FileLogger(std::wstring  logDirectoryW,
               std::wstring  baseFileName,
               ULONGLONG maxFileBytes,
               ULONGLONG rotateCount,
               ULONGLONG flushIntervalMs,
               ULONGLONG flushThresholdBytes,
               BOOL fsyncOnFlush);

    ~FileLogger() override;

    void consume(const std::vector<LogRecord>& batch) override;

    void flush() override;

    void close();

    ULONGLONG droppedCount() const;

private:
    void writerLoop(const std::stop_token &stoken);

    void openLogFile();

    BOOL writeBufferToDisk(std::string const& buffer);

    void rotateFile();

    BOOL ensureDirectoryExists() const;

    static std::string
    toUtf8(std::wstring const& wstr);

    std::wstring m_logDirectoryW;
    std::wstring m_baseFileName;
    ULONGLONG m_maxFileBytes;
    ULONGLONG m_rotateCount;
    ULONGLONG m_flushIntervalMs;
    ULONGLONG m_flushThresholdBytes;
    BOOL m_fsyncOnFlush;

    std::jthread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue; 
    std::string m_buffer; 
    HANDLE m_fileHandle{ INVALID_HANDLE_VALUE };
    std::atomic<ULONGLONG> m_dropped{ 0 };
    std::atomic<BOOL> m_running{ false };
    std::atomic<ULONGLONG> m_currentFileBytes{ 0 };

    std::wstring m_currentFilePathW;
    ULONGLONG m_rotationSerial{ 0 }; 
};

} // namespace core::logging
