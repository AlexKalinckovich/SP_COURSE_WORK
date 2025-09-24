// FileLogger.cpp
#include "FileLogger.h"

#include <chrono>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <utility>
#include <shlobj.h>

using namespace core::logging;


namespace core::logging
{

static std::wstring makeFilePath(std::wstring const& dirW, std::wstring const& nameW)
{
    std::wstring path = dirW;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
    {
        path.push_back(L'\\');
    }
    path += nameW;
    return path;
}

std::string FileLogger::toUtf8(const std::wstring& wstr)
{
    if (wstr.empty())
    {
        return {};
    }

    const int required_size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                                  static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);

    if (required_size <= 0)
    {
        const DWORD error = GetLastError();
        std::cerr << "FileLogger: WideCharToMultiByte failed, error=" << error << "\n";
        return {};
    }

    std::string result(static_cast<size_t>(required_size), '\0');

    const int converted_size = WideCharToMultiByte(CP_UTF8,0,wstr.c_str(),
                                                   static_cast<int>(wstr.size()), result.data(), required_size, nullptr,
                                                   nullptr);

    if (converted_size <= 0)
    {
        const DWORD error = GetLastError();
        std::cerr << "FileLogger: WideCharToMultiByte conversion failed, error=" << error << "\n";
        return {};
    }

    if (static_cast<size_t>(converted_size) < result.size())
    {
        result.resize(static_cast<size_t>(converted_size));
    }

    return result;
}


FileLogger::FileLogger(std::wstring logDirectoryW,
                       std::wstring baseFileName,
                       const ULONGLONG maxFileBytes,
                       const ULONGLONG rotateCount,
                       const ULONGLONG flushIntervalMs,
                       const ULONGLONG flushThresholdBytes,
                       const BOOL fsyncOnFlush)
    : m_logDirectoryW(std::move(logDirectoryW))
    , m_baseFileName(std::move(baseFileName))
    , m_maxFileBytes(maxFileBytes)
    , m_rotateCount(rotateCount)
    , m_flushIntervalMs(flushIntervalMs)
    , m_flushThresholdBytes(flushThresholdBytes)
    , m_fsyncOnFlush(fsyncOnFlush)
{
    const BOOL ok = ensureDirectoryExists();
    if (!ok)
    {
        std::cerr << "FileLogger: cannot create log directory\n";
    }

    openLogFile();

    m_running.store(true);
    m_worker = std::jthread([this](const std::stop_token& st)
    {
        this->writerLoop(st);
    });
}

FileLogger::~FileLogger()
{
    try
    {
        close();
    }
    catch (...)
    {}
}

// ---------- ensure directory ----------
BOOL FileLogger::ensureDirectoryExists() const
{
    const int result = SHCreateDirectoryExW(nullptr, m_logDirectoryW.c_str(), nullptr);

    switch (result)
    {
        case ERROR_SUCCESS:
        case ERROR_ALREADY_EXISTS:
            return TRUE;
        case ERROR_FILE_EXISTS:
            std::cerr << "FileLogger: path exists but is a file: '"
                    << toUtf8(m_logDirectoryW) << "'\n";
            return FALSE;
        default:
            std::cerr << "FileLogger: failed to create directory, error=" << result << "\n";
            return FALSE;
    }
}

BOOL FileLogger::openLogFile()
{
    std::lock_guard file_lk(m_file_mutex);

    const std::wstring file_path = makeFilePath(m_logDirectoryW, m_baseFileName);


    HANDLE file = CreateFileW(
        file_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        std::cerr << "FileLogger: failed to open file, error=" << error << "\n";

        if (error == ERROR_ACCESS_DENIED || error == ERROR_PATH_NOT_FOUND)
        {
            m_fileHandle = INVALID_HANDLE_VALUE;
            return FALSE;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        file = CreateFileW(file_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            m_fileHandle = INVALID_HANDLE_VALUE;
            return FALSE;
        }
    }

    LARGE_INTEGER new_position;
    new_position.QuadPart = 0;

    if (!SetFilePointerEx(file, new_position, nullptr, FILE_END))
    {
        std::cerr << "FileLogger: warning - cannot seek to end, writing from beginning\n";
    }

    LARGE_INTEGER file_size;
    if (GetFileSizeEx(file, &file_size))
    {
        m_currentFileBytes.store(static_cast<ULONGLONG>(file_size.QuadPart));
    }
    else
    {
        m_currentFileBytes.store(0);
    }

    m_fileHandle = file;
    m_currentFilePathW = file_path;

    return TRUE;
}

void FileLogger::consume(const std::vector<LogRecord>& batch)
{
    if (!m_running.load(std::memory_order_acquire))
    {
        m_dropped.fetch_add(batch.size(), std::memory_order_relaxed);
        return;
    }

    std::vector<std::string> lines;
    lines.reserve(batch.size());

    ULONGLONG total_batch_size = 0;
    for (const LogRecord &record: batch)
    {
        std::string line = record.toNDJsonLine();
        total_batch_size += line.size();
        lines.push_back(std::move(line));
    }
    {
        std::lock_guard lk(m_mutex);

        const ULONGLONG threshold = m_flushThresholdBytes * 10;
        const ULONGLONG current_size = m_currentQueueSize;

        if (current_size + total_batch_size > threshold)
        {
            const ULONGLONG need_to_free = (current_size + total_batch_size) - threshold;
            ULONGLONG freed_size = 0;

            while (!m_queue.empty() && freed_size < need_to_free)
            {
                freed_size += m_queue.front().size();
                m_queue.pop_front();
            }

            const ULONGLONG dropped_count = (freed_size + total_batch_size - need_to_free) /
                                            (lines.empty() ? 1 : lines[0].size());
            m_dropped.fetch_add(dropped_count, std::memory_order_relaxed);
            m_currentQueueSize -= freed_size;
        }

        for (std::string &line: lines)
        {
            m_queue.push_back(std::move(line));
        }
        m_currentQueueSize += total_batch_size;
    }

    m_cv.notify_one();
}

void FileLogger::writerLoop(const std::stop_token& stoken)
{
    using namespace std::chrono_literals;
    const auto flushInterval = std::chrono::milliseconds(m_flushIntervalMs);

    std::string local_buffer;
    local_buffer.reserve(m_flushThresholdBytes * 2);

    while (!stoken.stop_requested() || !m_queue.empty())
    {
        bool immediate_flush = false;
        {
            ULONGLONG extracted_size = 0;
            std::unique_lock lk(m_mutex);

            if (m_immediate_flush_requested)
            {
                immediate_flush = true;
                m_immediate_flush_requested = false;
            }

            if (m_queue.empty())
            {
                m_cv.wait_for(lk, flushInterval, [this, &stoken]
                {
                    return !m_queue.empty() || stoken.stop_requested();
                });
            }

            while (!m_queue.empty() && local_buffer.size() < m_flushThresholdBytes)
            {
                local_buffer.append(m_queue.front());
                extracted_size += m_queue.front().size();
                m_queue.pop_front();
            }

            m_currentQueueSize -= extracted_size;
        }

        if (!local_buffer.empty() || immediate_flush)
        {
            const BOOL ok = writeBufferToDisk(local_buffer);
            if (!ok)
            {
                const ULONGLONG approximate_records_lost = local_buffer.size() / 100;
                m_dropped.fetch_add(approximate_records_lost, std::memory_order_relaxed);
            }
            local_buffer.clear();
            local_buffer.reserve(m_flushThresholdBytes * 2);
        }
        else if (!stoken.stop_requested())
        {
            std::this_thread::yield();
        }
    }

    if (!local_buffer.empty())
    {
        writeBufferToDisk(local_buffer);
    }
}

BOOL FileLogger::writeBufferToDisk(const std::string& buffer)
{
    if (m_fileHandle == INVALID_HANDLE_VALUE)
    {
        if (!openLogFile())
        {
            return FALSE;
        }
    }

    const char *data = buffer.data();
    size_t remaining = buffer.size();
    constexpr DWORD kMaxSingleWrite = 64 * 1024;

    while (remaining > 0)
    {
        const DWORD to_write = std::min(remaining, static_cast<size_t>(kMaxSingleWrite));
        DWORD written = 0;

        BOOL write_ok = WriteFile(m_fileHandle, data, to_write, &written, nullptr);
        if (!write_ok || written != to_write)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_DISK_FULL || error == ERROR_ACCESS_DENIED)
            {
                std::cerr << "FileLogger critical error: " << error << "\n";
                return FALSE;
            }

            if (!recoverFromWriteError(error))
            {
                return FALSE;
            }

            write_ok = WriteFile(m_fileHandle, data, to_write, &written, nullptr);
            if (!write_ok)
            {
                return FALSE;
            }
        }

        data += written;
        remaining -= written;

        const ULONGLONG new_size = m_currentFileBytes.fetch_add(written) + written;
        if (new_size >= m_maxFileBytes)
        {
            if (!rotateFile())
            {
                return FALSE;
            }
        }
    }

    return conditionalFlush();
}

BOOL FileLogger::recoverFromWriteError(DWORD error)
{
    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return openLogFile();
}

BOOL FileLogger::conditionalFlush() const
{
    if (!m_fsyncOnFlush) return TRUE;

    static std::chrono::steady_clock::time_point last_flush;
    const auto now = std::chrono::steady_clock::now();

    if (now - last_flush < std::chrono::seconds(1)) {
        return TRUE;
    }

    last_flush = now;
    return FlushFileBuffers(m_fileHandle);
}

BOOL FileLogger::rotateFile()
{
    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        if (m_fsyncOnFlush)
        {
            FlushFileBuffers(m_fileHandle);
        }
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }

    SYSTEMTIME utc_time;
    GetSystemTime(&utc_time);

    std::wstring new_name;
    try
    {
        std::wostringstream oss;
        oss << m_baseFileName << L'.'
                << std::setw(4) << utc_time.wYear << L'-'
                << std::setw(2) << utc_time.wMonth << L'-'
                << std::setw(2) << utc_time.wDay << L'T'
                << std::setw(2) << utc_time.wHour << L'-'
                << std::setw(2) << utc_time.wMinute << L'-'
                << std::setw(2) << utc_time.wSecond << L'.'
                << std::setw(6) << utc_time.wMilliseconds << L'Z'
                << L".log";
        new_name = oss.str();
    } catch (...)
    {
        new_name = m_baseFileName + L".rotated." + std::to_wstring(++m_rotationSerial) + L".log";
    }

    const std::wstring new_path = makeFilePath(m_logDirectoryW, new_name);
    BOOL success = FALSE;

    success = MoveFileW(m_currentFilePathW.c_str(), new_path.c_str());

    if (!success && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        const std::wstring unique_path = new_path + L".unique";
        success = MoveFileW(m_currentFilePathW.c_str(), unique_path.c_str());

    }

    if (!success)
    {
        const DWORD error = GetLastError();
        std::cerr << "FileLogger: rotate failed, error=" << error
                << ", continuing with current file\n";

        return openLogFile();
    }

    cleanupOldFiles();

    m_currentFileBytes.store(0);
    return openLogFile();
}


void FileLogger::cleanupOldFiles() const
{
    if (m_rotateCount == 0) return;
    const std::wstring search_pattern = makeFilePath(m_logDirectoryW, m_baseFileName + L".*");

    try
    {
        WIN32_FIND_DATAW find_data;
        HANDLE find_handle = FindFirstFileW(search_pattern.c_str(), &find_data);

        if (find_handle == INVALID_HANDLE_VALUE) return;

        std::vector<std::wstring> rotated_files;

        do
        {
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            std::wstring filename = find_data.cFileName;
            if (filename.find(m_baseFileName) == 0 && filename != m_baseFileName)
            {
                rotated_files.push_back(makeFilePath(m_logDirectoryW, filename));
            }
        } while (FindNextFileW(find_handle, &find_data));

        FindClose(find_handle);

        std::sort(rotated_files.begin(), rotated_files.end(),
                  [](const std::wstring &a, const std::wstring &b)
                  {
                      return GetFileTimeValue(a) < GetFileTimeValue(b);
                  });

        if (rotated_files.size() > m_rotateCount)
        {
            for (size_t i = 0; i < rotated_files.size() - m_rotateCount; ++i)
            {
                DeleteFileW(rotated_files[i].c_str());
            }
        }
    } catch (...) {}
}

ULONGLONG FileLogger::GetFileTimeValue(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA file_attr;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file_attr))
    {
        return (static_cast<ULONGLONG>(file_attr.ftLastWriteTime.dwHighDateTime) << 32) |
               file_attr.ftLastWriteTime.dwLowDateTime;
    }
    return 0;
}

void FileLogger::flush()
{
    std::atomic<bool> flush_in_progress{true}; {
        std::lock_guard lk(m_mutex);
        m_immediate_flush_requested = true;
    }
    m_cv.notify_one(); {
        std::mutex flush_mutex;
        std::condition_variable flush_cv;
        std::unique_lock lk(flush_mutex);
        const bool completed = flush_cv.wait_for(lk, std::chrono::seconds(5), [this, &flush_in_progress]
        {
            std::lock_guard inner_lk(m_mutex);
            return m_queue.empty() && m_buffer.empty() && !flush_in_progress.load();
        });

        if (!completed)
        {
            std::cerr << "FileLogger: flush timeout after 5 seconds\n";
        }
    }

    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        std::lock_guard file_lk(m_file_mutex);

        BOOL flush_ok = TRUE;
        if (m_fsyncOnFlush)
        {
            flush_ok = FlushFileBuffers(m_fileHandle);
        }

        if (!flush_ok)
        {
            const DWORD error = GetLastError();
            std::cerr << "FileLogger: flush failed, error=" << error << "\n";
        }
    }
}


// ---------- close ----------
void FileLogger::close()
{
    BOOL expected = true;
    if (!m_running.compare_exchange_strong(expected, false))
    {
        return;
    }

    if (m_worker.joinable())
    {
        m_worker.request_stop();
        m_cv.notify_all();

        const auto timeout = std::chrono::seconds(10);
        const auto start = std::chrono::steady_clock::now();

        while (m_worker.joinable())
        {
            if (std::chrono::steady_clock::now() - start > timeout)
            {
                std::cerr << "FileLogger: worker thread timeout, forcing termination\n";
                m_worker.detach();
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50)); {
                std::lock_guard lk(m_mutex);
                if (m_queue.empty())
                {
                    if (m_worker.joinable())
                    {
                        m_worker.join();
                    }
                    break;
                }
            }
        }
    }

    std::string final_data; {
        std::lock_guard lk(m_mutex);
        for (std::string &line: m_queue)
        {
            final_data.append(line);
        }
        m_queue.clear();
        final_data.append(m_buffer);
        m_buffer.clear();
    }

    if (!final_data.empty())
    {
        writeBufferToDisk(final_data);
    }

    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        std::lock_guard file_lk(m_file_mutex);

        if (m_fsyncOnFlush)
        {
            FlushFileBuffers(m_fileHandle);
        }

        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }
}

ULONGLONG FileLogger::droppedCount() const
{
    return m_dropped.load();
}

} // namespace core::logging
