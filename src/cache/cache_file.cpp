#include "cache_file.h"

#include <QDir>
#include <QFileInfo>

#include <new>

namespace cadcontour::cache {
namespace {
std::optional<std::uint64_t> position(QIODevice &device)
{
    const auto p = device.pos();
    return p < 0 ? std::nullopt : std::optional{static_cast<std::uint64_t>(p)};
}
Error ioError(QIODevice &device, ErrorCode code, Operation operation,
              std::optional<std::uint64_t> offset = std::nullopt)
{
    const auto file = qobject_cast<QFileDevice *>(&device);
    return makeError(code, Phase::FileIO, operation, device.errorString(), offset,
                     file ? static_cast<int>(file->error()) : 0);
}
Status checkTransfer(QIODevice &device, std::size_t size, bool writing)
{
    if (!device.isOpen() || (writing ? !device.isWritable() : !device.isReadable()))
        return makeError(ErrorCode::InvalidInput, Phase::FileIO, writing ? Operation::Write : Operation::Read,
                         QStringLiteral("Device is not open in the required mode"), position(device));
    const auto start = position(device);
    const auto end = checked::add(start.value_or(0), size);
    if (!end || !checked::fileOffset(*end))
        return makeError(ErrorCode::InvalidInput, Phase::FileIO, writing ? Operation::Write : Operation::Read,
                         QStringLiteral("Transfer exceeds file offset range"), start);
    return success();
}
Error stateError()
{
    return makeError(ErrorCode::InvalidInput, Phase::Publication, Operation::Publish,
                     QStringLiteral("Publication attempt is terminal"));
}
} // namespace

namespace file {
Status readExact(QIODevice &device, MutableBytes bytes)
{
    if (auto s = checkTransfer(device, bytes.size(), false); !s) return s;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto offset = position(device);
        const auto requested = static_cast<qint64>(bytes.size() - done);
        const auto n = device.read(reinterpret_cast<char *>(bytes.data() + done), requested);
        if (n < 0 || n > requested) return ioError(device, ErrorCode::ReadFailed, Operation::Read, offset);
        if (n == 0)
            return ioError(device, device.atEnd() ? ErrorCode::IncompleteCache : ErrorCode::ReadFailed,
                           Operation::Read, offset);
        done += static_cast<std::size_t>(n);
    }
    return success();
}
Status writeExact(QIODevice &device, Bytes bytes)
{
    if (auto s = checkTransfer(device, bytes.size(), true); !s) return s;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto offset = position(device);
        const auto requested = static_cast<qint64>(bytes.size() - done);
        const auto n = device.write(reinterpret_cast<const char *>(bytes.data() + done), requested);
        if (n <= 0 || n > requested) return ioError(device, ErrorCode::WriteFailed, Operation::Write, offset);
        done += static_cast<std::size_t>(n);
    }
    return success();
}
Status seek(QIODevice &device, std::uint64_t offset)
{
    const auto signedOffset = checked::fileOffset(offset);
    if (!signedOffset)
        return makeError(ErrorCode::InvalidInput, Phase::FileIO, Operation::Seek,
                         QStringLiteral("Offset exceeds qint64"), offset);
    if (!device.isOpen() || device.isSequential() || !device.seek(*signedOffset))
        return ioError(device, ErrorCode::SeekFailed, Operation::Seek, offset);
    return success();
}
Result<std::uint64_t> size(QFileDevice &device)
{
    if (!device.isOpen()) return ioError(device, ErrorCode::ReadFailed, Operation::Size);
    const auto size = device.size();
    if (size < 0 || device.error() != QFileDevice::NoError)
        return ioError(device, ErrorCode::ReadFailed, Operation::Size);
    return static_cast<std::uint64_t>(size);
}
Status flush(QFileDevice &device)
{
    if (!device.isOpen() || !device.flush())
        return ioError(device, ErrorCode::FlushFailed, Operation::Flush, position(device));
    return success();
}
} // namespace file

PublishedFile::PublishedFile(std::unique_ptr<QTemporaryFile> file, QString path) noexcept
    : file_(std::move(file)), path_(std::move(path)) {}
QString PublishedFile::path() const noexcept { return file_ ? path_ : QString{}; }
QString PublishedFile::releaseToOwner() noexcept
{
    const auto name = path(); // prepare the returned path while cleanup still owns the file
    if (file_) {
        file_->setAutoRemove(false);
        file_.reset();
    }
    return name;
}

StagedFile::StagedFile(std::unique_ptr<QTemporaryFile> file, QString finalPath)
    : file_(std::move(file)), finalPath_(std::move(finalPath)) {}

Result<std::unique_ptr<StagedFile>> StagedFile::create(const QString &path)
{
    const QFileInfo destination(path);
    if (path.isEmpty() || destination.fileName().isEmpty() || destination.fileName() == QStringLiteral(".")
        || destination.fileName() == QStringLiteral("..") || !destination.dir().exists())
        return makeError(ErrorCode::InvalidInput, Phase::FileIO, Operation::Open,
                         QStringLiteral("Destination requires an existing directory and a file name"));
    try {
        auto file = std::make_unique<QTemporaryFile>(destination.absoluteDir().filePath(QStringLiteral(".pc2d-XXXXXX.tmp")));
        if (!file->open()) return ioError(*file, ErrorCode::OpenFailed, Operation::Open);
        return std::unique_ptr<StagedFile>(new StagedFile(std::move(file), destination.absoluteFilePath()));
    } catch (const std::bad_alloc &) {
        return makeError(ErrorCode::ResourceExhausted, Phase::FileIO, Operation::Open,
                         QStringLiteral("Cannot allocate temporary file owner"));
    }
}
QString StagedFile::temporaryPath() const { return file_ ? file_->fileName() : QString{}; }
Status StagedFile::writableState() const
{
    if (firstError_) return *firstError_;
    if (terminal_) return stateError();
    return success();
}
Status StagedFile::remember(Status result)
{
    if (!result && !firstError_) firstError_ = result.error();
    return firstError_ ? Status{*firstError_} : result;
}
Status StagedFile::write(Bytes bytes)
{
    if (auto s = writableState(); !s) return s;
    return remember(file::writeExact(*file_, bytes));
}
Status StagedFile::read(MutableBytes bytes)
{
    if (auto s = writableState(); !s) return s;
    return remember(file::readExact(*file_, bytes));
}
Status StagedFile::seek(std::uint64_t offset)
{
    if (auto s = writableState(); !s) return s;
    return remember(file::seek(*file_, offset));
}
Result<std::uint64_t> StagedFile::size()
{
    if (auto s = writableState(); !s) return s.error();
    auto result = file::size(*file_);
    if (!result) remember(result.error());
    return result;
}
Status StagedFile::flush()
{
    if (auto s = writableState(); !s) return s;
    return remember(flush_(*file_));
}
Result<PublishedFile> StagedFile::publish(std::uint64_t expected, Validator validator)
{
    if (auto s = writableState(); !s) return s.error();
    terminal_ = true;
    auto fail = [this](Error error) -> Result<PublishedFile> {
        remember(error);
        return *firstError_;
    };
    if (!validator || !checked::fileOffset(expected))
        return fail(makeError(ErrorCode::InvalidInput, Phase::Publication, Operation::Publish,
                              QStringLiteral("Publication requires validator and representable size")));
    if (auto s = flush_(*file_); !s) return fail(s.error());
    auto length = file::size(*file_);
    if (!length) return fail(length.error());
    if (length.value() != expected)
        return fail(makeError(ErrorCode::IncompleteCache, Phase::Publication, Operation::Size,
                              QStringLiteral("Unexpected file size")));
    if (auto s = file::seek(*file_, 0); !s) return fail(s.error());
    if (auto s = validator(*file_, expected); !s) return fail(s.error());
    // Keep autoRemove enabled across rename and the noexcept transfer to guard.
    if (!file_->rename(finalPath_)) {
        auto e = ioError(*file_, ErrorCode::PublishFailed, Operation::Publish);
        e.phase = Phase::Publication;
        return fail(std::move(e));
    }
    return PublishedFile{std::move(file_), finalPath_};
}

} // namespace cadcontour::cache
