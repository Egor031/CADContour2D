#pragma once
#include "measurement.h"
#include <QFile>
#include <memory>

namespace density::formats {
using Source = std::function<Tile(TileKey)>;
using Check = std::function<void()>;
U64 columns(const Spec &s);
U64 logicalTiles(const Spec &s);
U64 rasterOrdinal(const Spec &s, TileKey key);
Tile blank(const Spec &s, TileKey key);
void validateTile(const Spec &s, const Tile &t, TileKey key);
Plan storagePlan(const Spec &s, bool tiff);
QJsonObject versions();

// QFile calls are counted for both adapters, including libtiff client callbacks.
struct TrackedFile {
    QFile file;
    FileIo io;
    TrackedFile(const QString &path, bool write);
    QByteArray read(U64 offset, U64 bytes);
    void write(const QByteArray &bytes);
    void seek(U64 offset);
    void finish();
};
void writeD(const QString &path, const Spec &s, const Source &source, FileIo &io, const Check &check);
class DReader {
public:
    DReader(const QString &path, const Spec &s, U64 indexBudget);
    Tile read(TileKey key);
    std::vector<TileKey> physicalKeys() const;
    U64 metadataBytes() const;
    TrackedFile file;
private:
    struct Entry { U64 ordinal, offset, length, raw; std::uint32_t crc; };
    Spec spec;
    std::vector<Entry> entries;
};

// Deliberately dense TIFF tile arrays; missing offsets are errors, never zero data.
void writeE(const QString &path, const Spec &s, const Source &source, FileIo &io,
            const Check &check, bool big = true);
class EReader {
public:
    EReader(const QString &path, const Spec &s, U64 libraryBudget);
    ~EReader();
    Tile read(TileKey key);
    std::vector<TileKey> physicalKeys() const;
    U64 metadataBytes() const;
    TrackedFile &tracked();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace density::formats
