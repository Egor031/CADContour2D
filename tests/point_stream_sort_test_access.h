#pragma once
#include "cache/point_stream_sort.h"
#include <QDir>
#include <array>

namespace cadcontour::cache {
class PointStreamSortTestAccess {
public:
    static auto stats(const PointStreamSort &s) { return s.statistics_; }
    static std::size_t capacity(const PointStreamSort &s) { return s.runCapacity_; }
    static std::size_t block(const PointStreamSort &s) { return s.blockRecords_; }
    static std::size_t fan(const PointStreamSort &s) { return s.fanIn_; }
    static std::uint64_t fixed(const PointStreamSort &s) { return s.fixedBytes_; }
    static Phase phase(const PointStreamSort &s) { return s.phase_; }
    static QString path(const PointStreamSort &s) { return s.directory_->path(); }
    static QFile *source(PointStreamSort &s) { return s.source_.get(); }
    static QFile *destination(PointStreamSort &s) { return s.destination_.get(); }
    static void hook(PointStreamSort &s, Status (*fn)(PointStreamSort &, Operation, QFile &)) { s.ioHook_ = fn; }
    static void setCount(PointStreamSort &s, std::uint64_t n) { s.metadata_.pointCount = n; }
    static Status addEmptyRun(PointStreamSort &s)
    {
        if (!s.source_) {
            if (auto r = s.open(s.source_, QDir(s.directory_->path()).filePath(QStringLiteral("a.runs"))); !r) return r;
        }
        std::array<std::byte, 32> header{};
        binary::putU64(header, 0, 0x534e555244324350ULL);
        binary::putU64(header, 24, 14695981039346656037ULL); // FNV of empty payload
        if (auto r = file::writeExact(*s.source_, header); !r) return r;
        s.generation_.bytes += header.size(); ++s.generation_.runs;
        return success();
    }
};
} // namespace cadcontour::cache
