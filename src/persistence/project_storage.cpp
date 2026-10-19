#include "project_storage.h"

#include <QDir>
#include <QFileInfo>

#define NOMINMAX
#include <Windows.h>

namespace cadcontour::persistence {
namespace {
Error invalid(QString detail)
{
    return {ErrorCode::InvalidReference, QStringLiteral("path"), std::move(detail)};
}
QString cacheUuid(const cache::CacheId &id)
{
    return QUuid::fromRfc4122(QByteArrayView(reinterpret_cast<const char *>(id.bytes.data()), 16))
        .toString(QUuid::WithoutBraces);
}
bool reserved(const QString &component)
{
    const auto stem = component.section(u'.', 0, 0).trimmed().toUpper();
    if (stem == u"CON" || stem == u"PRN" || stem == u"AUX" || stem == u"NUL"
        || stem == u"CONIN$" || stem == u"CONOUT$") return true;
    if (stem.size() == 4 && (stem.startsWith(u"COM") || stem.startsWith(u"LPT"))) {
        const auto c = stem[3];
        return (c >= u'1' && c <= u'9') || c == QChar(0x00b9) || c == QChar(0x00b2) || c == QChar(0x00b3);
    }
    return false;
}
} // namespace

QString pointCacheRelativePath(QUuid storageId, const cache::CacheId &cacheId)
{
    return QStringLiteral("data/") + storageId.toString(QUuid::WithoutBraces)
        + QStringLiteral("/datasets/") + cacheUuid(cacheId) + QStringLiteral(".pc2dcache");
}

Status validateDatasetPath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(u'/') || path.contains(u'\\') || path.contains(u':'))
        return invalid(QStringLiteral("Expected a portable relative path using forward slashes"));
    const auto components = path.split(u'/');
    for (const auto &part : components) {
        if (part.isEmpty() || part == u"." || part == u".." || part.endsWith(u'.')
            || part.endsWith(u' ') || reserved(part) || part.size() > 255)
            return invalid(QStringLiteral("Empty, ambiguous or reserved path component"));
        for (qsizetype i = 0; i < part.size(); ++i) {
            const auto c = part[i];
            if (c.unicode() < 32 || c == u'<' || c == u'>' || c == u'"'
                || c == u'|' || c == u'?' || c == u'*')
                return invalid(QStringLiteral("Invalid Windows filename character"));
            if (c.isHighSurrogate()) {
                if (++i >= part.size() || !part[i].isLowSurrogate())
                    return invalid(QStringLiteral("Invalid Unicode path"));
            } else if (c.isLowSurrogate()) return invalid(QStringLiteral("Invalid Unicode path"));
        }
    }
    return std::monostate{};
}

Result<ResolvedDatasetPath> resolveDatasetPath(const QString &directory, const QString &relative)
{
    if (auto s = validateDatasetPath(relative); !s) return s.error();
    const QFileInfo root(directory);
    const auto canonical = root.canonicalFilePath();
    if (!root.isAbsolute() || !root.isDir() || canonical.isEmpty())
        return invalid(QStringLiteral("Project root must be an accessible absolute directory"));
    auto current = canonical;
    const auto parts = relative.split(u'/');
    bool exists = true;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current += u'/' + parts[i];
        const auto native = QDir::toNativeSeparators(current).toStdWString();
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                return Error{ErrorCode::FileIO, QStringLiteral("path"),
                    QStringLiteral("Cannot inspect dataset path (Win32 %1)").arg(code)};
            exists = false;
            continue; // Still inspect the remaining prefix; no path is created.
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return invalid(QStringLiteral("Dataset path crosses a reparse point"));
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory != (i + 1 < parts.size()))
            return invalid(QStringLiteral("Expected parent directories and a final regular file"));
    }
    return ResolvedDatasetPath{current, exists};
}

} // namespace cadcontour::persistence
