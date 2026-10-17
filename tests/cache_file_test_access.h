#pragma once
#include "cache/cache_file.h"

namespace cadcontour::cache {
class CacheFileTestAccess {
public:
    static void setFlush(StagedFile &file, Status (*flush)(QFileDevice &)) { file.flush_ = flush; }
    static void close(StagedFile &file) { file.file_->close(); }
};
} // namespace cadcontour::cache
