#pragma once

#include "binary_codec.h"

#include <QFileDevice>
#include <QIODevice>
#include <QTemporaryFile>

#include <memory>

namespace cadcontour::cache {

namespace file {
Status readExact(QIODevice &device, MutableBytes bytes);
Status writeExact(QIODevice &device, Bytes bytes);
Status seek(QIODevice &device, std::uint64_t offset);
Result<std::uint64_t> size(QFileDevice &device);
Status flush(QFileDevice &device);
} // namespace file

class StagedFile;
class CacheFileTestAccess;

// Owns cleanup after rename, until the application explicitly takes the file.
// This is a file guard, not a PendingCache or a domain dataset.
class PublishedFile {
public:
    PublishedFile(PublishedFile &&) noexcept = default;
    PublishedFile &operator=(PublishedFile &&) noexcept = default;
    PublishedFile(const PublishedFile &) = delete;
    PublishedFile &operator=(const PublishedFile &) = delete;
    QString path() const noexcept;
    // Caller becomes responsible for the persistent file. No deletion on destruction.
    QString releaseToOwner() noexcept;
private:
    friend class StagedFile;
    friend class CacheFileTestAccess;
    PublishedFile(std::unique_ptr<QTemporaryFile> file, QString path) noexcept;
    std::unique_ptr<QTemporaryFile> file_;
    QString path_;
};

class StagedFile {
public:
    using Validator = Status (*)(QIODevice &, std::uint64_t expectedSize);
    using ContextValidator = Status (*)(QIODevice &, std::uint64_t expectedSize, void *context);
    static Result<std::unique_ptr<StagedFile>> create(const QString &finalPath);
    StagedFile(const StagedFile &) = delete;
    StagedFile &operator=(const StagedFile &) = delete;
    ~StagedFile() = default;

    QString temporaryPath() const;
    Status write(Bytes bytes);
    Status read(MutableBytes bytes);
    Status seek(std::uint64_t offset);
    Result<std::uint64_t> size();
    Status flush();
    // Explicit terminal cleanup with diagnostics; destructor remains a fallback.
    Status discard();
    // Terminal attempt (also on validation failure). Validator must only read the staged device,
    // returns structured failure and must check the caller's required structure.
    Result<PublishedFile> publish(std::uint64_t expectedSize, Validator validator);
    // Context is borrowed only for this synchronous call (e.g. buffers and cancellation).
    Result<PublishedFile> publish(std::uint64_t expectedSize, ContextValidator validator, void *context);
    const std::optional<Error> &firstError() const noexcept { return firstError_; }
private:
    friend class CacheFileTestAccess;
    StagedFile(std::unique_ptr<QTemporaryFile> file, QString finalPath);
    Status remember(Status result);
    Status writableState() const;

    std::unique_ptr<QTemporaryFile> file_;
    QString finalPath_;
    std::optional<Error> firstError_;
    bool terminal_ = false;
    Status (*flush_)(QFileDevice &) = file::flush;
};

} // namespace cadcontour::cache
