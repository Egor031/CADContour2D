#pragma once
#include "cache/cache_file.h"
#include "cache/cache_writer.h"

namespace cadcontour::cache {
class CacheFileTestAccess {
public:
    static QTemporaryFile *guard(PendingCache &cache) { return cache.file_.file_.get(); }
    static void setFlush(StagedFile &file, Status (*flush)(QFileDevice &)) { file.flush_ = flush; }
    static void close(StagedFile &file) { file.file_->close(); }
};
} // namespace cadcontour::cache
