// FileLogger.cpp
#include "FileLogger.h"

#include <chrono>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <utility>

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

std::string FileLogger::toUtf8(std::wstring const& wstr)
{
    if (wstr.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return {};
    }

    std::string result;
    result.resize(static_cast<std::size_t>(required));
    const int converted = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()),
                                              result.data(), required, nullptr, nullptr);
    if (converted <= 0)
    {
        return {};
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
    , m_worker()
    , m_fileHandle(INVALID_HANDLE_VALUE)
    , m_running(false)
    , m_currentFileBytes(0)
{
    const BOOL ok = ensureDirectoryExists();
    if (!ok)
    {
        std::cerr << "FileLogger: cannot create log directory\n";
    }

    openLogFile();

    m_running.store(true);
    m_worker = std::jthread([this](std::stop_token st)
    {
        this->writerLoop(std::move(st));
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
    BOOL result = false;
    const DWORD attrs = GetFileAttributesW(m_logDirectoryW.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
    {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            result = true;
        }
    }
    else if (const BOOL created = CreateDirectoryW(m_logDirectoryW.c_str(), nullptr); created == FALSE)
    {
        if (const DWORD err = GetLastError(); err == ERROR_ALREADY_EXISTS)
        {
            result = true;
        }
    }
    return result;
}

void FileLogger::openLogFile()
{
    m_currentFilePathW = makeFilePath(m_logDirectoryW, m_baseFileName);

    HANDLE file = CreateFileW(m_currentFilePathW.c_str(),
                              GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        std::cerr << "FileLogger: CreateFileW failed\n";
        m_fileHandle = INVALID_HANDLE_VALUE;
        m_currentFileBytes.store(0);
        return;
    }

    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    const BOOL setOk = SetFilePointerEx(file, zero, nullptr, FILE_END);
    if (setOk == FALSE)
    {
        CloseHandle(file);
        file = CreateFileW(m_currentFilePathW.c_str(),
                           GENERIC_WRITE | GENERIC_READ,
                           FILE_SHARE_READ,
                           nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            std::cerr << "FileLogger: failed to reopen log file\n";
            m_fileHandle = INVALID_HANDLE_VALUE;
            m_currentFileBytes.store(0);
            return;
        }
    }

    LARGE_INTEGER size;
    const BOOL sizeOk = GetFileSizeEx(file, &size);
    if (sizeOk == FALSE)
    {
        m_currentFileBytes.store(0);
    }
    else
    {
        m_currentFileBytes.store(static_cast<ULONGLONG>(size.QuadPart));
    }

    m_fileHandle = file;
}

void FileLogger::consume(const std::vector<LogRecord>& batch)
{
    if (!m_running.load())
    {
        m_dropped.fetch_add(1);
        return;
    }

    std::lock_guard<std::mutex> lk(m_mutex);

    for (const LogRecord& i : batch)
    {
        std::string line = i.toNDJsonLine();
        m_queue.push_back(line);

        const ULONG threshold = m_flushThresholdBytes * 10u;
        ULONG qsizeApprox = 0u;
        for (const std::string& j : m_queue)
        {
            qsizeApprox += static_cast<ULONG>(j.size());
            if (qsizeApprox > threshold)
            {
                break;
            }
        }
        if (qsizeApprox > threshold)
        {
            m_queue.pop_front();
            m_dropped.fetch_add(1);
        }
    }

    m_cv.notify_one();
}

void FileLogger::writerLoop(const std::stop_token &stoken)
{
    using namespace std::chrono_literals;
    std::chrono::milliseconds flushInterval(static_cast<int>(m_flushIntervalMs));

    while (!stoken.stop_requested() || !m_queue.empty())
    {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            if (m_queue.empty())
            {
                m_cv.wait_for(lk, flushInterval);
            }

            while (!m_queue.empty() && m_buffer.size() < m_flushThresholdBytes)
            {
                m_buffer.append(m_queue.front());
                m_queue.pop_front();
            }

        }

        if (!m_buffer.empty())
        {
            const BOOL ok = writeBufferToDisk(m_buffer);
            if (!ok)
            {
                m_dropped.fetch_add(1);
            }
            m_buffer.clear();
        }

        if (stoken.stop_requested())
        {
        }
    }

    if (!m_buffer.empty())
    {
        writeBufferToDisk(m_buffer);
        m_buffer.clear();
    }

    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }
}

BOOL FileLogger::writeBufferToDisk(std::string const& buffer)
{
    if (m_fileHandle == INVALID_HANDLE_VALUE)
    {
        openLogFile();
        if (m_fileHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
    }

    const char* data = buffer.data();
    std::size_t remaining = buffer.size();
    DWORD written = 0;

    while (remaining > 0u)
    {
        const DWORD toWrite = (remaining > static_cast<std::size_t>(0xFFFFFFFFu) ? 0xFFFFFFFFu : static_cast<DWORD>(remaining));
        if (BOOL ok = WriteFile(m_fileHandle, data, toWrite, &written, nullptr); ok == FALSE)
        {
            const DWORD err = GetLastError();
            std::cerr << "FileLogger WriteFile failed error=" << err << "\n";
            CloseHandle(m_fileHandle);
            m_fileHandle = INVALID_HANDLE_VALUE;
            openLogFile();
            if (m_fileHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            ok = WriteFile(m_fileHandle, data, toWrite, &written, nullptr);
            if (ok == FALSE)
            {
                const DWORD err2 = GetLastError();
                std::cerr << "FileLogger WriteFile retry failed error=" << err2 << "\n";
                return false;
            }
        }

        data += written;
        remaining -= static_cast<std::size_t>(written);
        m_currentFileBytes.fetch_add(static_cast<ULONGLONG>(written));

        if (m_currentFileBytes.load() >= m_maxFileBytes)
        {
            if (m_fsyncOnFlush && m_fileHandle != INVALID_HANDLE_VALUE)
            {
                FlushFileBuffers(m_fileHandle);
            }
            rotateFile();
        }
    }

    if (m_fsyncOnFlush && m_fileHandle != INVALID_HANDLE_VALUE)
    {
        const BOOL flushed = FlushFileBuffers(m_fileHandle);
        if (flushed == FALSE)
        {
            const DWORD ferr = GetLastError();
            std::cerr << "FileLogger FlushFileBuffers failed error=" << ferr << "\n";
            return false;
        }
    }

    return true;
}

void FileLogger::rotateFile()
{
    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wostringstream oss;
    oss << m_baseFileName << L'.'
        << std::setw(4) << std::setfill(L'0') << st.wYear << L'-'
        << std::setw(2) << std::setfill(L'0') << st.wMonth << L'-'
        << std::setw(2) << std::setfill(L'0') << st.wDay << L'_'
        << std::setw(2) << std::setfill(L'0') << st.wHour << L'-'
        << std::setw(2) << std::setfill(L'0') << st.wMinute << L'-'
        << std::setw(2) << std::setfill(L'0') << st.wSecond << L'.'
        << m_rotationSerial++;

    const std::wstring rotatedNameW = oss.str();
    const std::wstring rotatedPathW = makeFilePath(m_logDirectoryW, rotatedNameW);

    const BOOL moved = MoveFileExW(m_currentFilePathW.c_str(), rotatedPathW.c_str(), MOVEFILE_COPY_ALLOWED);
    if (moved == FALSE)
    {
        const DWORD err = GetLastError();
        std::cerr << "FileLogger rotate MoveFileExW failed error=" << err << "\n";
        const BOOL moved2 = MoveFileW(m_currentFilePathW.c_str(), rotatedPathW.c_str());
        if (moved2 == FALSE)
        {
            const DWORD err2 = GetLastError();
            std::cerr << "FileLogger rotate MoveFileW fallback failed error=" << err2 << "\n";
            const std::wstring fallbackName = m_baseFileName + L".rot";
            const std::wstring fallbackPath = makeFilePath(m_logDirectoryW, fallbackName);
            MoveFileExW(m_currentFilePathW.c_str(), fallbackPath.c_str(), MOVEFILE_COPY_ALLOWED);
        }
    }

    openLogFile();

}

void FileLogger::flush()
{
    while (true)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_queue.empty() && m_buffer.empty())
            {
                break;
            }
        }
        m_cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (m_fileHandle != INVALID_HANDLE_VALUE && m_fsyncOnFlush)
    {
        const BOOL ok = FlushFileBuffers(m_fileHandle);
        if (ok == FALSE)
        {
            const DWORD err = GetLastError();
            std::cerr << "FileLogger flush FlushFileBuffers failed error=" << err << "\n";
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
    }

    m_cv.notify_all();

    if (m_fileHandle != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(m_fileHandle);
        CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }
}

ULONGLONG FileLogger::droppedCount() const
{
    return m_dropped.load();
}

} // namespace core::logging
