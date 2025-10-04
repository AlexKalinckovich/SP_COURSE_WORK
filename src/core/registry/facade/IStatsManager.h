// IStatsManager.h
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex>

namespace core::registry
{

struct PerformanceStats {
    size_t cacheHits = 0;
    size_t cacheMisses = 0;
    size_t keysOpened = 0;
    size_t valuesRead = 0;
    size_t valuesWritten = 0;
    std::chrono::milliseconds totalOperationTime{0};

    [[nodiscard]] double GetCacheHitRatio() const {
        const size_t total = cacheHits + cacheMisses;
        size_t result;
        if(total > 0)
        {
            result = cacheHits / total;
        }
        else
        {
            result = 0.0F;
        }
        return static_cast<double>(result);
    }

    [[nodiscard]] size_t GetTotalOperations() const {
        return keysOpened + valuesRead + valuesWritten;
    }

    [[nodiscard]] std::chrono::milliseconds GetAverageOperationTime() const {
        const size_t totalOps = GetTotalOperations();
        return totalOps > 0 ? totalOperationTime / totalOps : std::chrono::milliseconds(0);
    }

    PerformanceStats& operator+=(const PerformanceStats& other) {
        cacheHits += other.cacheHits;
        cacheMisses += other.cacheMisses;
        keysOpened += other.keysOpened;
        valuesRead += other.valuesRead;
        valuesWritten += other.valuesWritten;
        totalOperationTime += other.totalOperationTime;
        return *this;
    }
};

class IStatsManager {
public:
    virtual ~IStatsManager() = default;

    virtual void RecordCacheHit() = 0;
    virtual void RecordCacheMiss() = 0;
    virtual void RecordKeyOpened() = 0;
    virtual void RecordValueRead() = 0;
    virtual void RecordValueWritten() = 0;
    virtual void RecordOperationTime(std::chrono::milliseconds duration) = 0;

    [[nodiscard]] virtual PerformanceStats GetStats() const = 0;
    virtual void ResetStats() = 0;

    [[nodiscard]] virtual bool IsEnabled() const = 0;
    virtual void SetEnabled(bool enabled) = 0;
};

class StatsManagerImpl final : public IStatsManager {
public:
    StatsManagerImpl();
    ~StatsManagerImpl() override;

    void RecordCacheHit() override;
    void RecordCacheMiss() override;
    void RecordKeyOpened() override;
    void RecordValueRead() override;
    void RecordValueWritten() override;
    void RecordOperationTime(std::chrono::milliseconds duration) override;

    PerformanceStats GetStats() const override;
    void ResetStats() override;

    bool IsEnabled() const override;
    void SetEnabled(bool enabled) override;

private:
    struct ThreadLocalStats {
        std::chrono::milliseconds operationTime{0};

        ThreadLocalStats() = default;
        ~ThreadLocalStats() = default;
    };

    std::atomic<size_t> m_cacheHits{0};
    std::atomic<size_t> m_cacheMisses{0};
    std::atomic<size_t> m_keysOpened{0};
    std::atomic<size_t> m_valuesRead{0};
    std::atomic<size_t> m_valuesWritten{0};

    static thread_local std::unique_ptr<ThreadLocalStats> m_threadLocalStats;

    mutable std::shared_mutex m_timeMutex;
    std::chrono::milliseconds m_globalOperationTime{0};

    std::atomic<bool> m_enabled{true};

    ThreadLocalStats& GetThreadLocalStats();
    void FlushThreadLocalStats() const;
};

} // namespace core::registry