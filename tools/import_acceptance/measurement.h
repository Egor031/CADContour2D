#pragma once

#include <QDirIterator>
#include <QFile>
#include <QJsonObject>
#include <QSet>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

inline void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
inline void number(QJsonObject &object, const char *key, std::uint64_t value)
{
    object[key] = QString::number(value);
}
inline double since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
inline QSet<QString> filesIn(const QString &root)
{
    QSet<QString> paths;
    QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) paths.insert(files.next());
    return paths;
}

// Samples are collected away from the GUI thread. EOF sums are logical file
// lengths, not allocated clusters or physical disk traffic. Old files are excluded.
class Measurement {
public:
    Measurement(QString root, const QString &samples)
        : root_(std::move(root)), oldFiles_(filesIn(root_))
    {
        QFile reserve(samples);
        require(reserve.open(QIODevice::WriteOnly | QIODevice::NewOnly), "samples already exist or cannot be created");
        reserve.close();
        require(GetProcessIoCounters(GetCurrentProcess(), &before_) != 0, "GetProcessIoCounters failed");
        thread_ = std::thread([this, samples] {
            QFile output(samples);
            if (!output.open(QIODevice::WriteOnly | QIODevice::Append)) { apiFailure_ = true; return; }
            output.write("ms,working_set_bytes,private_bytes,new_files_eof_bytes\n");
            do {
                PROCESS_MEMORY_COUNTERS_EX memory{};
                memory.cb = sizeof(memory);
                if (!GetProcessMemoryInfo(GetCurrentProcess(),
                        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
                    apiFailure_ = true;
                    break;
                }
                peakWorkingSet_ = std::max<std::uint64_t>(peakWorkingSet_, memory.PeakWorkingSetSize);
                sampledWorkingSet_ = std::max<std::uint64_t>(sampledWorkingSet_, memory.WorkingSetSize);
                sampledPrivate_ = std::max<std::uint64_t>(sampledPrivate_, memory.PrivateUsage);
                peakPagefile_ = std::max<std::uint64_t>(peakPagefile_, memory.PeakPagefileUsage);
                std::uint64_t disk = 0;
                for (const auto &path : filesIn(root_)) {
                    if (oldFiles_.contains(path)) continue;
                    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file == INVALID_HANDLE_VALUE) continue;
                    LARGE_INTEGER size{};
                    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0) disk += std::uint64_t(size.QuadPart);
                    CloseHandle(file);
                }
                peakDisk_ = std::max(peakDisk_, disk);
                const auto line = QByteArray::number(since(start_), 'f', 3) + ','
                    + QByteArray::number(qulonglong(memory.WorkingSetSize)) + ','
                    + QByteArray::number(qulonglong(memory.PrivateUsage)) + ','
                    + QByteArray::number(qulonglong(disk)) + '\n';
                if (output.write(line) != line.size()) apiFailure_ = true;
                ++samples_;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } while (!stop_);
            if (!output.flush()) apiFailure_ = true;
        });
    }
    ~Measurement() { stop(); }
    void stop()
    {
        if (thread_.joinable()) {
            stop_ = true;
            thread_.join();
            if (!GetProcessIoCounters(GetCurrentProcess(), &after_)) apiFailure_ = true;
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
                peakWorkingSet_ = std::max<std::uint64_t>(peakWorkingSet_, memory.PeakWorkingSetSize);
                peakPagefile_ = std::max<std::uint64_t>(peakPagefile_, memory.PeakPagefileUsage);
                sampledWorkingSet_ = std::max<std::uint64_t>(sampledWorkingSet_, memory.WorkingSetSize);
                sampledPrivate_ = std::max<std::uint64_t>(sampledPrivate_, memory.PrivateUsage);
            } else apiFailure_ = true;
        }
    }
    QJsonObject json()
    {
        stop();
        QJsonObject result{{"sampleIntervalMs", 100}, {"nativeApiOrSampleFailure", apiFailure_}};
        number(result, "sampleCount", samples_);
        number(result, "processLifetimePeakWorkingSetBytes", peakWorkingSet_);
        number(result, "sampledPeakWorkingSetBytes", sampledWorkingSet_);
        number(result, "sampledPeakPrivateBytes", sampledPrivate_);
        number(result, "processLifetimePeakPagefileUsageBytes", peakPagefile_);
        number(result, "sampledPeakNewFilesEofBytes", peakDisk_);
        number(result, "logicalReadBytes", after_.ReadTransferCount - before_.ReadTransferCount);
        number(result, "logicalWriteBytes", after_.WriteTransferCount - before_.WriteTransferCount);
        number(result, "logicalReadOperations", after_.ReadOperationCount - before_.ReadOperationCount);
        number(result, "logicalWriteOperations", after_.WriteOperationCount - before_.WriteOperationCount);
        number(result, "remainingNewFiles", std::uint64_t((filesIn(root_) - oldFiles_).size()));
        return result;
    }
private:
    QString root_;
    QSet<QString> oldFiles_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::atomic<bool> stop_ = false;
    std::thread thread_;
    bool apiFailure_ = false;
    std::uint64_t peakWorkingSet_ = 0, sampledWorkingSet_ = 0, sampledPrivate_ = 0;
    std::uint64_t peakPagefile_ = 0, peakDisk_ = 0, samples_ = 0;
    IO_COUNTERS before_{}, after_{};
};

QJsonObject validateCache(const QString &path, const QString &scratch, const QString &oraclePath);
