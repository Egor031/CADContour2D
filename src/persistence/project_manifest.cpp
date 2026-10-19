#include "project_manifest.h"
#include "project_storage.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace cadcontour::persistence {
namespace {
[[noreturn]] void fail(ErrorCode code, QString field, QString detail)
{
    throw Error{code, std::move(field), std::move(detail)};
}
Error resource()
{
    return {ErrorCode::ResourceExhausted, {}, QStringLiteral("Manifest admission budget exhausted")};
}
QString uuidText(const auto &id)
{
    return QUuid::fromRfc4122(QByteArrayView(reinterpret_cast<const char *>(id.bytes.data()), 16))
        .toString(QUuid::WithoutBraces);
}
QUuid uuid(const QString &text, const QString &field)
{
    const QUuid value(text);
    if (value.isNull() || value.toString(QUuid::WithoutBraces) != text)
        fail(ErrorCode::InvalidValue, field, QStringLiteral("Expected a nonzero canonical lowercase UUID"));
    return value;
}
template<class Id> Id idValue(QUuid value)
{
    Id id;
    const auto bytes = value.toRfc4122();
    std::memcpy(id.bytes.data(), bytes.constData(), id.bytes.size());
    return id;
}
bool validText(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].unicode() == 0) return false;
        if (text[i].isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
    }
    return true;
}
QString decimal(double value)
{
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) fail(ErrorCode::NumericRange, {}, QStringLiteral("Binary64 encoding failed"));
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}
bool digit(char c) { return c >= '0' && c <= '9'; }
bool numericStart(char c) { return digit(c) || c == '-' || c == '+' || c == '.'; }
bool decimalSyntax(const QByteArray &s, bool integer)
{
    qsizetype i = 0;
    if (!integer && i < s.size() && s[i] == '-') ++i;
    if (i == s.size() || !digit(s[i])) return false;
    if (s[i++] != '0') while (i < s.size() && digit(s[i])) ++i;
    if (!integer && i < s.size() && s[i] == '.') {
        const auto start = ++i;
        while (i < s.size() && digit(s[i])) ++i;
        if (i == start) return false;
    }
    if (!integer && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        const auto start = i;
        while (i < s.size() && digit(s[i])) ++i;
        if (i == start) return false;
    }
    return i == s.size();
}
template<class T> T number(const QString &text, const QString &field)
{
    const auto s = text.toLatin1();
    if (!decimalSyntax(s, std::is_integral_v<T>))
        fail(ErrorCode::InvalidValue, field, QStringLiteral("Expected a complete decimal string"));
    T value{};
    const auto converted = [&] {
        if constexpr (std::is_integral_v<T>) return std::from_chars(s.data(), s.data() + s.size(), value);
        else return std::from_chars(s.data(), s.data() + s.size(), value, std::chars_format::general);
    }();
    if (converted.ec == std::errc::result_out_of_range)
        fail(ErrorCode::NumericRange, field, QStringLiteral("Decimal outside representable range"));
    if (converted.ec != std::errc{} || converted.ptr != s.data() + s.size())
        fail(ErrorCode::InvalidValue, field, QStringLiteral("Invalid decimal value"));
    if constexpr (!std::is_integral_v<T>) {
        if (!std::isfinite(value)) fail(ErrorCode::NumericRange, field, QStringLiteral("Nonfinite binary64"));
    }
    return value;
}
QJsonValue required(const QJsonObject &object, const char *name, QJsonValue::Type type)
{
    const auto field = QString::fromLatin1(name);
    if (!object.contains(field)) fail(ErrorCode::MissingField, field, QStringLiteral("Required field is absent"));
    const auto value = object.value(field);
    if (value.type() != type) fail(ErrorCode::InvalidType, field, QStringLiteral("Unexpected JSON type"));
    return value;
}
QString string(const QJsonObject &o, const char *name)
{
    auto s = required(o, name, QJsonValue::String).toString();
    if (!validText(s)) fail(ErrorCode::InvalidValue, QString::fromLatin1(name), QStringLiteral("Invalid Unicode string"));
    return s;
}
QJsonObject object(const QJsonObject &o, const char *name)
{
    return required(o, name, QJsonValue::Object).toObject();
}
void fields(const QJsonObject &o, std::initializer_list<const char *> names)
{
    for (auto i = o.begin(); i != o.end(); ++i) {
        bool known = false;
        for (auto name : names) known |= i.key() == QLatin1StringView(name);
        if (!known) fail(ErrorCode::UnsupportedFormat, i.key(), QStringLiteral("Unknown semantic field"));
    }
}
void version(const QJsonObject &o, const char *name)
{
    const auto v = required(o, name, QJsonValue::Double).toDouble();
    if (!std::isfinite(v) || v < 0 || std::floor(v) != v || v > std::numeric_limits<std::uint32_t>::max())
        fail(ErrorCode::NumericRange, QString::fromLatin1(name), QStringLiteral("Expected uint32 version"));
    if (v != 1) fail(ErrorCode::UnsupportedVersion, QString::fromLatin1(name), QStringLiteral("Only version 1 is supported"));
}

unsigned unicodeEscape(const QByteArray &bytes, qsizetype &position)
{
    if (bytes.size() - position < 4)
        fail(ErrorCode::InvalidJson, {}, QStringLiteral("Truncated Unicode escape"));
    unsigned value = 0;
    for (int n = 0; n < 4; ++n) {
        const auto c = bytes[position++];
        unsigned hex;
        if (digit(c)) hex = c - '0';
        else if (c >= 'a' && c <= 'f') hex = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hex = c - 'A' + 10;
        else fail(ErrorCode::InvalidJson, {}, QStringLiteral("Invalid Unicode escape digit"));
        value = (value << 4) | hex;
    }
    return value;
}
qsizetype stringEnd(const QByteArray &bytes, qsizetype start)
{
    auto position = start + 1;
    while (position < bytes.size()) {
        const auto c = static_cast<unsigned char>(bytes[position++]);
        if (c == '"') return position;
        if (c < 0x20) fail(ErrorCode::InvalidJson, {}, QStringLiteral("Unescaped JSON control character"));
        if (c != '\\') continue;
        if (position == bytes.size())
            fail(ErrorCode::InvalidJson, {}, QStringLiteral("Truncated JSON escape"));
        switch (bytes[position++]) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't': break;
        case 'u': {
            const auto unit = unicodeEscape(bytes, position);
            if (unit >= 0xd800 && unit <= 0xdbff) {
                if (bytes.size() - position < 2 || bytes[position] != '\\' || bytes[position + 1] != 'u')
                    fail(ErrorCode::InvalidJson, {}, QStringLiteral("Missing low surrogate escape"));
                position += 2;
                const auto low = unicodeEscape(bytes, position);
                if (low < 0xdc00 || low > 0xdfff)
                    fail(ErrorCode::InvalidJson, {}, QStringLiteral("Invalid low surrogate escape"));
            } else if (unit >= 0xdc00 && unit <= 0xdfff) {
                fail(ErrorCode::InvalidJson, {}, QStringLiteral("Unpaired low surrogate escape"));
            }
            break;
        }
        default: fail(ErrorCode::InvalidJson, {}, QStringLiteral("Invalid JSON string escape"));
        }
    }
    fail(ErrorCode::InvalidJson, {}, QStringLiteral("Unterminated JSON string"));
}

// Runs before Qt builds a DOM. Qt remains the JSON grammar parser; this pass bounds
// structure and rejects permissive string/number parsing and duplicate decoded keys.
void preflight(const QByteArray &bytes, ManifestLimits limits)
{
    if (!limits.maxBytes || !limits.maxTokens || !limits.maxDepth
        || static_cast<std::uint64_t>(bytes.size()) > limits.maxBytes) throw resource();
    if (!bytes.isValidUtf8()) fail(ErrorCode::InvalidJson, {}, QStringLiteral("Invalid UTF-8"));
    struct Frame { char kind; QSet<QString> keys; };
    std::vector<Frame> stack;
    std::uint64_t tokens = 0;
    auto token = [&] { if (tokens == limits.maxTokens) throw resource(); ++tokens; };
    auto white = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    for (qsizetype i = 0; i < bytes.size();) {
        const auto c = bytes[i];
        if (white(c) || c == ':' || c == ',') { ++i; continue; }
        if (c == '{' || c == '[') {
            token();
            if (stack.size() >= limits.maxDepth) throw resource();
            stack.push_back({c, {}}); ++i;
        } else if (c == '}' || c == ']') {
            if (stack.empty() || stack.back().kind != (c == '}' ? '{' : '['))
                fail(ErrorCode::InvalidJson, {}, QStringLiteral("Unbalanced JSON containers"));
            stack.pop_back(); ++i;
        } else if (c == '"') {
            token();
            const auto start = i;
            i = stringEnd(bytes, start);
            auto next = i;
            while (next < bytes.size() && white(bytes[next])) ++next;
            if (next < bytes.size() && bytes[next] == ':' && !stack.empty() && stack.back().kind == '{') {
                QJsonParseError parse;
                const auto keyDoc = QJsonDocument::fromJson('[' + bytes.mid(start, i - start) + ']', &parse);
                if (parse.error != QJsonParseError::NoError)
                    fail(ErrorCode::InvalidJson, {}, parse.errorString());
                const auto key = keyDoc.array().at(0).toString();
                if (stack.back().keys.contains(key))
                    fail(ErrorCode::InvalidJson, key, QStringLiteral("Duplicate JSON property"));
                stack.back().keys.insert(key);
                // Qt converts JSON numbers to binary64. Inspect version tokens before
                // that conversion can round a fractional/future value to integer 1.
                if (key == u"version" || key == u"formatVersion") {
                    auto startValue = next + 1;
                    while (startValue < bytes.size() && white(bytes[startValue])) ++startValue;
                    auto endValue = startValue;
                    while (endValue < bytes.size() && !white(bytes[endValue]) && bytes[endValue] != ','
                           && bytes[endValue] != '}' && bytes[endValue] != ']') ++endValue;
                    const auto raw = bytes.mid(startValue, endValue - startValue);
                    if (!raw.isEmpty() && numericStart(raw.front())) {
                        if (!decimalSyntax(raw, false))
                            fail(ErrorCode::InvalidJson, key, QStringLiteral("Malformed JSON number token"));
                        if (!decimalSyntax(raw, true))
                            fail(ErrorCode::NumericRange, key, QStringLiteral("Version requires an unsigned integer token"));
                        std::uint32_t value = 0;
                        const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                        if (converted.ec == std::errc::result_out_of_range)
                            fail(ErrorCode::NumericRange, key, QStringLiteral("Version outside uint32 range"));
                    }
                }
            }
        } else {
            const auto start = i;
            token(); ++i;
            while (i < bytes.size() && !white(bytes[i]) && bytes[i] != ',' && bytes[i] != ']'
                   && bytes[i] != '}' && bytes[i] != ':' && bytes[i] != '"'
                   && bytes[i] != '[' && bytes[i] != '{') ++i;
            if (numericStart(c) && !decimalSyntax(bytes.mid(start, i - start), false))
                fail(ErrorCode::InvalidJson, {}, QStringLiteral("Malformed JSON number token"));
        }
    }
    if (!stack.empty()) fail(ErrorCode::InvalidJson, {}, QStringLiteral("Unclosed JSON container"));
}
void validate(const ProjectSnapshot &s)
{
    if (s.projectId.isNull() || s.storageId.isNull() || s.snapshotId.isNull())
        fail(ErrorCode::InvalidValue, QStringLiteral("identity"), QStringLiteral("Null identity"));
    if (!ProjectState::isValidCell(s.cell))
        fail(ErrorCode::InvalidValue, QStringLiteral("cell"), QStringLiteral("Cell must be finite and positive"));
    if (s.source.path.isEmpty() || !validText(s.source.path) || !s.source.bytes || !s.source.lines)
        fail(ErrorCode::InconsistentState, QStringLiteral("source"), QStringLiteral("Invalid imported source metadata"));
    // v1 has exactly one imported point cache. No placeholder future dataset kinds.
    if (s.datasets.size() != 1 || s.pointCacheId.isNull())
        fail(ErrorCode::InconsistentState, QStringLiteral("datasets"), QStringLiteral("Expected one referenced point cache"));
    const auto &d = s.datasets.front();
    if (auto status = cache::validateMetadata(d.metadata); !status)
        fail(ErrorCode::InvalidValue, QStringLiteral("metadata"), status.error().detail);
    if (d.metadata.pointCount == 0 || d.metadata.pointCount > s.source.lines
        || s.source.lines > s.source.bytes || d.metadata.id != s.pointCacheId)
        fail(ErrorCode::InconsistentState, QStringLiteral("pointCacheId"), QStringLiteral("Inconsistent source/cache reference"));
    // Check representability of the complete v1 packed layout, without opening a file.
    auto pages = *cache::checked::pageCount(d.metadata.pointCount, cache::format::pointCapacity);
    std::uint64_t indexPages = 0;
    do {
        pages = *cache::checked::pageCount(pages, cache::format::indexCapacity);
        const auto sum = cache::checked::add(indexPages, pages);
        if (!sum) fail(ErrorCode::NumericRange, QStringLiteral("pointCount"), QStringLiteral("Cache layout overflow"));
        indexPages = *sum;
    } while (pages > 1);
    if (!cache::calculateLayout(d.metadata.pointCount, indexPages))
        fail(ErrorCode::NumericRange, QStringLiteral("pointCount"), QStringLiteral("Cache layout exceeds file range"));
    if (auto status = validateDatasetPath(d.relativePath); !status) throw status.error();
    if (d.relativePath != pointCacheRelativePath(s.storageId, d.metadata.id))
        fail(ErrorCode::InvalidReference, QStringLiteral("path"), QStringLiteral("Dataset path does not match storage/cache identities"));
}
QJsonObject json(const ProjectSnapshot &s)
{
    QJsonArray datasets;
    for (const auto &d : s.datasets) {
        const auto &m = d.metadata;
        datasets.append(QJsonObject{
            {"kind", "pointCache"}, {"formatVersion", 1}, {"cacheId", uuidText(m.id)}, {"path", d.relativePath},
            {"pointCount", QString::number(m.pointCount)}, {"hasNonZeroZ", m.hasNonZeroZ},
            {"bounds", QJsonObject{{"minX", decimal(m.bounds.minX)}, {"minY", decimal(m.bounds.minY)},
                                   {"maxX", decimal(m.bounds.maxX)}, {"maxY", decimal(m.bounds.maxY)}}}});
    }
    return {{"format", "CADContour2D"}, {"version", 1}, {"projectId", uuidText(s.projectId)},
        {"storageId", s.storageId.toString(QUuid::WithoutBraces)},
        {"snapshotId", s.snapshotId.toString(QUuid::WithoutBraces)}, {"units", "mm"},
        {"parameters", QJsonObject{{"cell", decimal(s.cell)}}},
        {"source", QJsonObject{{"path", s.source.path}, {"bytes", QString::number(s.source.bytes)},
                                {"lines", QString::number(s.source.lines)}}},
        {"state", QJsonObject{{"pointCacheId", uuidText(s.pointCacheId)}}}, {"datasets", datasets}};
}
} // namespace

Result<ProjectSnapshot> captureSnapshot(const ProjectState &state, QUuid storageId, QUuid snapshotId)
{
    try {
        if (!state.dataset()) return Error{ErrorCode::InconsistentState, {}, QStringLiteral("No imported dataset")};
        const auto &d = *state.dataset();
        ProjectSnapshot s{state.projectId(), storageId, snapshotId, state.cell(),
            {QString::fromStdWString(d.sourcePath.wstring()), d.sourceBytes, d.sourceLines}, d.cacheId,
            {{pointCacheRelativePath(storageId, d.cacheId),
              {d.cacheId, d.pointCount, {d.minimum.x, d.minimum.y, d.maximum.x, d.maximum.y}, d.hasNonZeroZ}}}};
        validate(s);
        return s;
    } catch (const Error &error) { return error; }
      catch (const std::bad_alloc &) { return resource(); }
}

Result<QByteArray> encodeManifest(const ProjectSnapshot &snapshot, ManifestLimits limits)
{
    try {
        validate(snapshot);
        // v1 contains a fixed number of fields. Bound its only unbounded informational
        // string before converting to UTF-8/building a DOM; final admission uses actual bytes.
        if (static_cast<std::uint64_t>(snapshot.source.path.size()) > limits.maxBytes) throw resource();
        auto bytes = QJsonDocument(json(snapshot)).toJson(QJsonDocument::Indented);
        preflight(bytes, limits);
        return bytes;
    } catch (const Error &error) { return error; }
      catch (const std::bad_alloc &) { return resource(); }
}

Result<ProjectSnapshot> decodeManifest(const QByteArray &utf8, ManifestLimits limits)
{
    try {
        preflight(utf8, limits);
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(utf8, &parse);
        if (parse.error != QJsonParseError::NoError) fail(ErrorCode::InvalidJson, {}, parse.errorString());
        if (!document.isObject()) fail(ErrorCode::InvalidType, {}, QStringLiteral("Expected manifest object"));
        const auto root = document.object();
        if (string(root, "format") != u"CADContour2D")
            fail(ErrorCode::UnsupportedFormat, QStringLiteral("format"), QStringLiteral("Invalid format signature"));
        version(root, "version");
        fields(root, {"format", "version", "projectId", "storageId", "snapshotId", "units", "parameters", "source", "state", "datasets"});
        if (string(root, "units") != u"mm")
            fail(ErrorCode::UnsupportedFormat, QStringLiteral("units"), QStringLiteral("Only millimetres are supported"));
        ProjectSnapshot s;
        s.projectId = idValue<ProjectId>(uuid(string(root, "projectId"), QStringLiteral("projectId")));
        s.storageId = uuid(string(root, "storageId"), QStringLiteral("storageId"));
        s.snapshotId = uuid(string(root, "snapshotId"), QStringLiteral("snapshotId"));
        const auto parameters = object(root, "parameters");
        fields(parameters, {"cell"});
        s.cell = number<double>(string(parameters, "cell"), QStringLiteral("cell"));
        const auto source = object(root, "source");
        fields(source, {"path", "bytes", "lines"});
        s.source = {string(source, "path"), number<std::uint64_t>(string(source, "bytes"), QStringLiteral("bytes")),
                    number<std::uint64_t>(string(source, "lines"), QStringLiteral("lines"))};
        const auto state = object(root, "state");
        fields(state, {"pointCacheId"});
        s.pointCacheId = idValue<cache::CacheId>(uuid(string(state, "pointCacheId"), QStringLiteral("pointCacheId")));
        const auto datasets = required(root, "datasets", QJsonValue::Array).toArray();
        if (datasets.size() != 1)
            fail(ErrorCode::InconsistentState, QStringLiteral("datasets"), QStringLiteral("Expected exactly one point cache"));
        for (const auto &value : datasets) {
            if (!value.isObject()) fail(ErrorCode::InvalidType, QStringLiteral("datasets"), QStringLiteral("Expected descriptor object"));
            const auto d = value.toObject();
            fields(d, {"kind", "formatVersion", "cacheId", "path", "pointCount", "hasNonZeroZ", "bounds"});
            if (string(d, "kind") != u"pointCache")
                fail(ErrorCode::UnsupportedFormat, QStringLiteral("kind"), QStringLiteral("Unknown dataset kind"));
            version(d, "formatVersion");
            const auto b = object(d, "bounds");
            fields(b, {"minX", "minY", "maxX", "maxY"});
            s.datasets.push_back({string(d, "path"),
                {idValue<cache::CacheId>(uuid(string(d, "cacheId"), QStringLiteral("cacheId"))),
                 number<std::uint64_t>(string(d, "pointCount"), QStringLiteral("pointCount")),
                 {number<double>(string(b, "minX"), QStringLiteral("minX")), number<double>(string(b, "minY"), QStringLiteral("minY")),
                  number<double>(string(b, "maxX"), QStringLiteral("maxX")), number<double>(string(b, "maxY"), QStringLiteral("maxY"))},
                 required(d, "hasNonZeroZ", QJsonValue::Bool).toBool()}});
        }
        validate(s);
        return s;
    } catch (const Error &error) { return error; }
      catch (const std::bad_alloc &) { return resource(); }
}

Result<ProjectSnapshot> readManifest(QIODevice &device, ManifestLimits limits)
{
    try {
        if (!limits.maxBytes || !limits.maxTokens || !limits.maxDepth) return resource();
        if (!device.isReadable() || device.isSequential() || device.pos() != 0 || device.isTextModeEnabled())
            return Error{ErrorCode::FileIO, {}, QStringLiteral("Expected readable nonsequential binary device at position zero")};
        const auto size = device.size();
        if (size < 0) return Error{ErrorCode::FileIO, {}, QStringLiteral("Cannot determine manifest size")};
        if (static_cast<std::uint64_t>(size) > limits.maxBytes
            || size > std::numeric_limits<qsizetype>::max() - 1) return resource();
        QByteArray bytes;
        std::array<char, 16384> chunk;
        while (true) {
            const auto remaining = limits.maxBytes - static_cast<std::uint64_t>(bytes.size());
            const auto request = remaining >= chunk.size() ? chunk.size() : static_cast<std::size_t>(remaining) + 1;
            const auto count = device.read(chunk.data(), static_cast<qint64>(request));
            if (count < 0) return Error{ErrorCode::FileIO, {}, device.errorString().left(256)};
            if (count == 0) break;
            if (static_cast<std::uint64_t>(count) > remaining
                || count > std::numeric_limits<qsizetype>::max() - bytes.size()) return resource();
            bytes.append(chunk.data(), static_cast<qsizetype>(count));
        }
        if (bytes.size() != size || device.size() != size)
            return Error{ErrorCode::FileIO, {}, QStringLiteral("Manifest changed while reading")};
        return decodeManifest(bytes, limits);
    } catch (const std::bad_alloc &) { return resource(); }
}

} // namespace cadcontour::persistence
