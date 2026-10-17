#include "cache/cache_writer.h"

#include <QCoreApplication>
#include <QThread>
#include <array>
#include <cstdio>

using namespace cadcontour::cache;

namespace {
QString checkpoint;
[[noreturn]] void ready()
{
    std::puts("READY");
    std::fflush(stdout);
    for (;;) QThread::msleep(1000); // Parent kills only this child, without stack unwinding.
}
}
namespace cadcontour::cache {
class CacheWriterTestAccess {
public:
    static void arm(CacheWriter &w) {
        w.ioHook_ = [](CacheWriter &writer, Operation op) -> Status {
            if (checkpoint == "before-seal" && writer.phase_ == Phase::PointPages
                && op == Operation::Write && writer.cacheBytes_ > format::headerSize) ready();
            if (checkpoint == "before-rename" && op == Operation::Publish) {
                if (auto s = writer.staged_->flush(); !s) return s;
                ready();
            }
            return success();
        };
    }
};
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 4) return 2;
    checkpoint = app.arguments()[1];
    auto writer = CacheWriter::create(app.arguments()[2], {1024 * 1024, 16 * 1024 * 1024, 4}, app.arguments()[3]);
    if (!writer) return 3;
    CacheWriterTestAccess::arm(*writer.value());
    std::array<Measurement, 6000> input{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = {double(i % 100), double(i / 100), 0};
    if (!writer.value()->append(input)) return 4;
    auto pending = writer.value()->finish();
    if (!pending) return 5;
    if (checkpoint == "after-rename") ready();
    if (checkpoint == "after-release") {
        const auto path = pending.value().releaseToOwner();
        if (path.isEmpty()) return 6;
        ready();
    }
    return 7;
}
