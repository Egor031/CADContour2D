#include "persistence/project_manifest.h"
#include "persistence/project_storage.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

#define NOMINMAX
#include <Windows.h>

using namespace cadcontour;
using namespace cadcontour::persistence;

namespace {
constexpr ManifestLimits limits{1024 * 1024, 4096, 32};
constexpr auto maxU64 = std::numeric_limits<std::uint64_t>::max();
ProjectId projectId()
{
    ProjectId id;
    const auto bytes = QUuid("e3fa43cb-04a2-42be-ae60-91263ea74616").toRfc4122();
    std::memcpy(id.bytes.data(), bytes.constData(), id.bytes.size());
    return id;
}
ProjectSnapshot snapshot()
{
    ProjectSnapshot s;
    s.projectId = projectId();
    s.storageId = QUuid("bd1422a6-a982-471b-800d-b466154a2637");
    s.snapshotId = QUuid("a5b3c605-a6c7-40af-94d3-4096eb536293");
    s.cell = std::nextafter(0.1, 1.0);
    s.source = {QStringLiteral("\\\\сервер/измерения/Деталь № 1.xyz"), 1234, 20};
    s.pointCacheId = cache::CacheId::create();
    s.datasets = {{pointCacheRelativePath(s.storageId, s.pointCacheId),
        {s.pointCacheId, 3, {-0.0, -1e200, 1e250, std::numeric_limits<double>::denorm_min()}, true}}};
    return s;
}
QJsonObject validJson()
{
    const auto bytes = encodeManifest(snapshot(), limits);
    EXPECT_TRUE(bytes);
    return bytes ? QJsonDocument::fromJson(bytes.value()).object() : QJsonObject{};
}
Result<ProjectSnapshot> decode(const QJsonObject &o)
{
    return decodeManifest(QJsonDocument(o).toJson(), limits);
}
void expectError(const QJsonObject &o, ErrorCode code)
{
    const auto result = decode(o);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code) << result.error().field.toStdString() << ": " << result.error().detail.toStdString();
}
void write(const QString &path)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write("deliberately not a cache"), 24);
}
auto bits(double value) { return std::bit_cast<std::uint64_t>(value); }

// Inject raw JSON after serialization so Qt cannot repair malformed test input.
QByteArray sourceTokenManifest(const QByteArray &token)
{
    auto o = validJson();
    auto source = o["source"].toObject(); source["path"] = "source-token-placeholder"; o["source"] = source;
    auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    bytes.replace("\"source-token-placeholder\"", token);
    return bytes;
}
QByteArray versionTokenManifest(const QByteArray &field, const QByteArray &token)
{
    auto bytes = QJsonDocument(validJson()).toJson(QJsonDocument::Compact);
    const auto original = '"' + field + "\":1";
    EXPECT_TRUE(bytes.contains(original));
    bytes.replace(original, '"' + field + "\":" + token);
    return bytes;
}
void expectRawError(const QByteArray &bytes, ErrorCode code = ErrorCode::InvalidJson)
{
    const auto result = decodeManifest(bytes, limits);
    if (result) {
        ADD_FAILURE() << "Accepted malformed JSON; source.path UTF-8 hex="
                      << result.value().source.path.toUtf8().toHex().toStdString()
                      << ", cell=" << result.value().cell;
    } else {
        EXPECT_EQ(result.error().code, code) << result.error().field.toStdString()
                                            << ": " << result.error().detail.toStdString();
    }
}
} // namespace

TEST(ManifestReviewRegression, H1RejectsInvalidStringEscapesInKeysAndValues)
{
    for (const auto &token : {R"("\q")", R"("\m")", R"("\1")", R"("\v")", R"("\a")",
                              R"("\x41")", R"("\'")", R"("\?")", R"("\U0041")"}) {
        SCOPED_TRACE(token);
        expectRawError(sourceTokenManifest(token));
    }
    auto units = QJsonDocument(validJson()).toJson(QJsonDocument::Compact);
    units.replace(R"("units":"mm")", R"("units":"m\m")");
    expectRawError(units);
    auto cell = validJson(); cell["parameters"] = QJsonObject{{"cell", "cell-token-placeholder"}};
    auto bytes = QJsonDocument(cell).toJson(QJsonDocument::Compact);
    bytes.replace(R"("cell-token-placeholder")", R"("\1")");
    expectRawError(bytes);
    auto key = QJsonDocument(validJson()).toJson(QJsonDocument::Compact);
    key.replace(R"("units")", R"("unit\s")");
    expectRawError(key);
}

TEST(ManifestReviewRegression, H1RejectsEveryUnescapedControlByte)
{
    for (int c = 0; c < 32; ++c) {
        SCOPED_TRACE(c);
        const auto token = QByteArray("\"before") + static_cast<char>(c) + "after\"";
        expectRawError(sourceTokenManifest(token));
    }
}

TEST(ManifestReviewRegression, H1RejectsMalformedUnicodeAndTruncatedStrings)
{
    for (const auto &token : {R"("\u")", R"("\u1")", R"("\u12")", R"("\u123")", R"("\u12G4")",
                              R"("\u{0041}")", R"("\uD800")", R"("\uDC00")", R"("\uDBFF\u0041")",
                              R"("\uD800\uD800")", R"("\uD800\n")", R"("\uDC00\uD800")",
                              "\"unterminated", "\"dangling\\", "\"escaped-closing\\\""}) {
        SCOPED_TRACE(token);
        expectRawError(sourceTokenManifest(token));
    }
}

TEST(ManifestReviewRegression, H1AcceptsStandardEscapesAndUnicodeWithoutChangingValues)
{
    struct Case { QByteArray token; QString expected; };
    for (const auto &item : {
        Case{R"("\"\\\/\b\f\n\r\t")", QStringLiteral("\"\\/\b\f\n\r\t")},
        Case{R"("\u0041\u00e9\u042F")", QStringLiteral("AéЯ")},
        Case{R"("\ud83d\uDE00")", QString::fromUcs4(U"😀")},
        Case{QStringLiteral("\"Деталь 😀.xyz\"").toUtf8(), QStringLiteral("Деталь 😀.xyz")},
        Case{R"("literal\\q")", QStringLiteral("literal\\q")}}) {
        SCOPED_TRACE(item.token.toStdString());
        const auto result = decodeManifest(sourceTokenManifest(item.token), limits);
        ASSERT_TRUE(result);
        EXPECT_EQ(result.value().source.path, item.expected);
    }
    auto o = validJson(); o["parameters"] = QJsonObject{{"cell", "1"}};
    auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    bytes.replace(R"("units":"mm")", R"("u\u006eits":"m\u006d")");
    bytes.replace(R"("cell":"1")", R"("cell":"\u0031")");
    const auto result = decodeManifest(bytes, limits);
    ASSERT_TRUE(result); EXPECT_EQ(bits(result.value().cell), bits(1.0));
    // Escaped NUL is valid JSON but remains forbidden by the source metadata contract.
    expectRawError(sourceTokenManifest(R"("\u0000")"), ErrorCode::InvalidValue);
}

TEST(ManifestReviewRegression, H2VersionTokensAndTypesAreStrict)
{
    for (const auto &field : {QByteArray("version"), QByteArray("formatVersion")}) {
        for (const auto &token : {QByteArray("1"), QByteArray(" \n1\t ")})
            EXPECT_TRUE(decodeManifest(versionTokenManifest(field, token), limits));
        struct Case { QByteArray token; ErrorCode code; };
        for (const auto &item : {
            Case{"1.", ErrorCode::InvalidJson}, Case{"01", ErrorCode::InvalidJson},
            Case{"+1", ErrorCode::InvalidJson}, Case{".1", ErrorCode::InvalidJson},
            Case{"1e", ErrorCode::InvalidJson}, Case{"1e+", ErrorCode::InvalidJson},
            Case{"1.e0", ErrorCode::InvalidJson}, Case{"1e-", ErrorCode::InvalidJson},
            Case{"1.0", ErrorCode::NumericRange}, Case{"1e0", ErrorCode::NumericRange},
            Case{"-1", ErrorCode::NumericRange}, Case{"-0", ErrorCode::NumericRange},
            Case{"1.00000000000000000001", ErrorCode::NumericRange},
            Case{"0", ErrorCode::UnsupportedVersion}, Case{"2", ErrorCode::UnsupportedVersion},
            Case{"4294967295", ErrorCode::UnsupportedVersion}, Case{"4294967296", ErrorCode::NumericRange},
            Case{QByteArray(400, '9'), ErrorCode::NumericRange},
            Case{"\"1\"", ErrorCode::InvalidType}, Case{"null", ErrorCode::InvalidType},
            Case{"true", ErrorCode::InvalidType}, Case{"[]", ErrorCode::InvalidType}, Case{"{}", ErrorCode::InvalidType}}) {
            SCOPED_TRACE(field.toStdString() + ": " + item.token.toStdString());
            expectRawError(versionTokenManifest(field, item.token), item.code);
        }
    }
}

TEST(ManifestReviewRegression, H2NumericGrammarIsCheckedBeforeFieldTypes)
{
    for (const auto &token : {QByteArray("1."), QByteArray("01"), QByteArray("+1"),
                              QByteArray(".1"), QByteArray("1e+"), QByteArray("1.e0")}) {
        auto bytes = QJsonDocument(validJson()).toJson(QJsonDocument::Compact);
        EXPECT_TRUE(bytes.contains(R"("hasNonZeroZ":true)"));
        bytes.replace(R"("hasNonZeroZ":true)", "\"hasNonZeroZ\":" + token);
        SCOPED_TRACE(token.toStdString());
        expectRawError(bytes);
    }
    auto bytes = QJsonDocument(validJson()).toJson(QJsonDocument::Compact);
    bytes.replace(R"("hasNonZeroZ":true)", R"("hasNonZeroZ":1)");
    expectRawError(bytes, ErrorCode::InvalidType);
}

TEST(Manifest, FullRoundTripAndExactFields)
{
    const auto original = snapshot();
    const auto encoded = encodeManifest(original, limits);
    ASSERT_TRUE(encoded);
    const auto decoded = decodeManifest(encoded.value(), limits);
    ASSERT_TRUE(decoded);
    const auto &s = decoded.value();
    EXPECT_EQ(s.projectId, original.projectId);
    EXPECT_EQ(s.storageId, original.storageId);
    EXPECT_EQ(s.snapshotId, original.snapshotId);
    EXPECT_EQ(s.pointCacheId, original.pointCacheId);
    EXPECT_EQ(bits(s.cell), bits(original.cell));
    EXPECT_EQ(s.source.path, original.source.path);
    EXPECT_EQ(s.source.bytes, original.source.bytes);
    EXPECT_EQ(s.source.lines, original.source.lines);
    ASSERT_EQ(s.datasets.size(), 1);
    EXPECT_EQ(s.datasets[0].relativePath, original.datasets[0].relativePath);
    EXPECT_EQ(s.datasets[0].metadata.id, original.pointCacheId);
    EXPECT_EQ(s.datasets[0].metadata.pointCount, 3);
    EXPECT_TRUE(s.datasets[0].metadata.hasNonZeroZ);
    const auto &b = s.datasets[0].metadata.bounds;
    const auto &a = original.datasets[0].metadata.bounds;
    EXPECT_EQ(bits(b.minX), bits(a.minX)); EXPECT_EQ(bits(b.minY), bits(a.minY));
    EXPECT_EQ(bits(b.maxX), bits(a.maxX)); EXPECT_EQ(bits(b.maxY), bits(a.maxY));
    const auto o = QJsonDocument::fromJson(encoded.value()).object();
    EXPECT_EQ(o.keys().size(), 10);
    EXPECT_EQ(o["format"].toString(), "CADContour2D");
    EXPECT_EQ(o["version"].toInt(), 1);
    EXPECT_EQ(o["units"].toString(), "mm");
    EXPECT_FALSE(encoded.value().contains("inputRevision"));
    EXPECT_FALSE(encoded.value().contains("runtimeIdentity"));
    EXPECT_FALSE(encoded.value().contains("OperationId"));
    const auto again = encodeManifest(s, limits);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), encoded.value());
}

TEST(Manifest, Binary64ExtremesAndSignedZerosAreBitExact)
{
    for (double v : {-std::numeric_limits<double>::max(), -1e200, -1e-200, -0.0, 0.0,
                     std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(),
                     std::nextafter(1.0, 0.0), std::nextafter(1.0, 2.0), std::numeric_limits<double>::max()}) {
        auto s = snapshot();
        s.datasets[0].metadata.bounds = {v, v, v, v};
        if (v > 0) s.cell = v;
        const auto bytes = encodeManifest(s, limits);
        ASSERT_TRUE(bytes);
        const auto result = decodeManifest(bytes.value(), limits);
        ASSERT_TRUE(result);
        const auto &b = result.value().datasets[0].metadata.bounds;
        EXPECT_EQ(bits(b.minX), bits(v)); EXPECT_EQ(bits(b.maxX), bits(v));
        EXPECT_EQ(bits(b.minY), bits(v)); EXPECT_EQ(bits(b.maxY), bits(v));
        EXPECT_EQ(bits(result.value().cell), bits(s.cell));
    }
}

TEST(Manifest, Uint64PrecisionAndRepresentablePointCount)
{
    for (auto value : {std::uint64_t{9007199254740991}, std::uint64_t{9007199254740993}, maxU64}) {
        auto s = snapshot(); s.source.bytes = value; s.source.lines = value;
        const auto bytes = encodeManifest(s, limits);
        ASSERT_TRUE(bytes);
        const auto result = decodeManifest(bytes.value(), limits);
        ASSERT_TRUE(result);
        EXPECT_EQ(result.value().source.bytes, value); EXPECT_EQ(result.value().source.lines, value);
    }
    auto s = snapshot();
    s.source = {"x.xyz", maxU64, maxU64};
    s.datasets[0].metadata.pointCount = maxU64;
    const auto overflow = encodeManifest(s, limits);
    ASSERT_FALSE(overflow); EXPECT_EQ(overflow.error().code, ErrorCode::NumericRange);
    // A count above exact JSON integer range still fits the actual v1 file layout.
    s.datasets[0].metadata.pointCount = 9007199254740993ULL;
    const auto bytes = encodeManifest(s, limits);
    ASSERT_TRUE(bytes);
    const auto result = decodeManifest(bytes.value(), limits);
    ASSERT_TRUE(result); EXPECT_EQ(result.value().datasets[0].metadata.pointCount, 9007199254740993ULL);
}

TEST(Manifest, EveryRequiredFieldAndType)
{
    const auto base = validJson();
    auto check = [](const QJsonObject &o, auto submit) {
        for (auto it = o.begin(); it != o.end(); ++it) {
            SCOPED_TRACE(it.key().toStdString());
            auto changed = o; changed.remove(it.key()); submit(changed, ErrorCode::MissingField);
            changed = o; changed[it.key()] = QJsonValue::Null; submit(changed, ErrorCode::InvalidType);
        }
    };
    check(base, [](auto o, auto code) { expectError(o, code); });
    for (const auto &name : {"parameters", "source", "state"}) {
        check(base[name].toObject(), [&](auto o, auto code) {
            auto root = base; root[name] = o; expectError(root, code);
        });
    }
    const auto d = base["datasets"].toArray()[0].toObject();
    check(d, [&](auto o, auto code) { auto root = base; root["datasets"] = QJsonArray{o}; expectError(root, code); });
    check(d["bounds"].toObject(), [&](auto o, auto code) {
        auto desc = d; desc["bounds"] = o;
        auto root = base; root["datasets"] = QJsonArray{desc}; expectError(root, code);
    });
}

TEST(Manifest, UnsupportedSignaturesVersionsUnitsAndKinds)
{
    auto o = validJson(); o["format"] = "other"; expectError(o, ErrorCode::UnsupportedFormat);
    o = validJson(); o["version"] = 2; expectError(o, ErrorCode::UnsupportedVersion);
    o["version"] = 1.25; expectError(o, ErrorCode::NumericRange);
    o["version"] = -1; expectError(o, ErrorCode::NumericRange);
    o["version"] = 1e100; expectError(o, ErrorCode::NumericRange);
    o = validJson(); o["units"] = "m"; expectError(o, ErrorCode::UnsupportedFormat);
    for (auto name : {"kind", "formatVersion"}) {
        o = validJson(); auto d = o["datasets"].toArray()[0].toObject();
        d[name] = name == QByteArray("kind") ? QJsonValue("density") : QJsonValue(2);
        o["datasets"] = QJsonArray{d};
        expectError(o, name == QByteArray("kind") ? ErrorCode::UnsupportedFormat : ErrorCode::UnsupportedVersion);
    }
}

TEST(Manifest, VersionTokensCannotBeRoundedToSupportedIntegers)
{
    const auto encoded = QJsonDocument(validJson()).toJson(QJsonDocument::Compact);
    for (const auto &field : {QByteArray("version"), QByteArray("formatVersion")}) {
        for (const auto &value : {QByteArray("1.00000000000000000001"), QByteArray("1e0"),
                                  QByteArray("1.0"), QByteArray("-0")}) {
            auto changed = encoded;
            const auto token = '"' + field + "\":1";
            ASSERT_TRUE(changed.contains(token));
            changed.replace(token, '"' + field + "\":" + value);
            const auto result = decodeManifest(changed, limits);
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::NumericRange);
        }
    }
}

TEST(Manifest, UnknownSemanticFieldsAreNeverDropped)
{
    for (auto name : {"", "parameters", "source", "state", "descriptor", "bounds"}) {
        auto o = validJson();
        if (*name == '\0') o["future"] = 1;
        else if (QByteArray(name) == "descriptor" || QByteArray(name) == "bounds") {
            auto d = o["datasets"].toArray()[0].toObject();
            if (QByteArray(name) == "bounds") { auto b = d["bounds"].toObject(); b["future"] = 1; d["bounds"] = b; }
            else d["future"] = 1;
            o["datasets"] = QJsonArray{d};
        } else { auto section = o[name].toObject(); section["future"] = 1; o[name] = section; }
        expectError(o, ErrorCode::UnsupportedFormat);
    }
}

TEST(Manifest, RejectsMalformedJsonUtf8AndDuplicateDecodedKeys)
{
    for (const auto &bytes : {QByteArray("{"), QByteArray("{} garbage"), QByteArray("{\"x\":\"unterminated"),
        QByteArray("{\"a\":1,\"a\":2}"), QByteArray("{\"a\":1,\"\\u0061\":2}"),
        QByteArray("{\"x\":\"\xff\"}")}) {
        const auto result = decodeManifest(bytes, limits);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::InvalidJson);
    }
    const auto result = decodeManifest("[]", limits);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::InvalidType);
}

TEST(Manifest, UuidsMustBeCanonicalNonzeroAndReferencesConsistent)
{
    for (auto name : {"projectId", "storageId", "snapshotId"}) {
        for (auto value : {"bad", "00000000-0000-0000-0000-000000000000", "{e3fa43cb-04a2-42be-ae60-91263ea74616}",
                           "E3FA43CB-04A2-42BE-AE60-91263EA74616"}) {
            auto o = validJson(); o[name] = value; expectError(o, ErrorCode::InvalidValue);
        }
    }
    auto o = validJson(); auto state = o["state"].toObject();
    state["pointCacheId"] = "bad"; o["state"] = state; expectError(o, ErrorCode::InvalidValue);
    state["pointCacheId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    o["state"] = state; expectError(o, ErrorCode::InconsistentState);
    o = validJson(); auto a = o["datasets"].toArray(); a.append(a[0]);
    o["datasets"] = a; expectError(o, ErrorCode::InconsistentState);
    o["datasets"] = QJsonArray{}; expectError(o, ErrorCode::InconsistentState);
    o["datasets"] = QJsonArray{1}; expectError(o, ErrorCode::InvalidType);
    o = validJson(); auto d = o["datasets"].toArray()[0].toObject();
    d["cacheId"] = "00000000-0000-0000-0000-000000000000";
    o["datasets"] = QJsonArray{d}; expectError(o, ErrorCode::InvalidValue);
}

TEST(Manifest, InvalidNumbersOverflowAndNonfinite)
{
    for (auto value : {"NaN", "nan", "inf", "-inf", " 1", "+1", "1x", "0x1p0", "01", "1.", "1e", "1\n"}) {
        auto o = validJson(); o["parameters"] = QJsonObject{{"cell", value}}; expectError(o, ErrorCode::InvalidValue);
    }
    for (auto value : {"1e999", "1e-9999"}) {
        auto o = validJson(); o["parameters"] = QJsonObject{{"cell", value}}; expectError(o, ErrorCode::NumericRange);
    }
    for (auto value : {"0", "-0", "-1"}) {
        auto o = validJson(); o["parameters"] = QJsonObject{{"cell", value}}; expectError(o, ErrorCode::InvalidValue);
    }
    auto o = validJson(); auto src = o["source"].toObject();
    src["bytes"] = "18446744073709551616"; o["source"] = src; expectError(o, ErrorCode::NumericRange);
    for (auto value : {"-1", "1.5", "1e3", "01", "+1"}) {
        src["bytes"] = value; o["source"] = src; expectError(o, ErrorCode::InvalidValue);
    }
    auto s = snapshot(); s.cell = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(encodeManifest(s, limits));
    s = snapshot(); s.datasets[0].metadata.bounds.minX = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(encodeManifest(s, limits));
}

TEST(Manifest, InconsistentSourceBoundsAndPath)
{
    for (int mode = 0; mode != 8; ++mode) {
        auto s = snapshot();
        if (mode == 0) s.source.bytes = 0;
        if (mode == 1) s.source.lines = 0;
        if (mode == 2) s.datasets[0].metadata.pointCount = 0;
        if (mode == 3) s.datasets[0].metadata.pointCount = 21;
        if (mode == 4) s.datasets[0].metadata.bounds.maxY = -std::numeric_limits<double>::max();
        if (mode == 5) s.datasets[0].relativePath = "../outside";
        if (mode == 6) s.datasets[0].relativePath = "data/different.pc2dcache";
        if (mode == 7) s.source.lines = s.source.bytes + 1;
        EXPECT_FALSE(encodeManifest(s, limits)) << mode;
    }
}

TEST(Manifest, AdmissionBudgetsBeforeDomAndBeforeFileRead)
{
    const auto bytes = encodeManifest(snapshot(), limits);
    ASSERT_TRUE(bytes);
    for (auto budget : {ManifestLimits{0, 1000, 32}, ManifestLimits{100, 1000, 32},
                        ManifestLimits{1024 * 1024, 1, 32}, ManifestLimits{1024 * 1024, 4096, 1}}) {
        const auto result = decodeManifest(bytes.value(), budget);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
        EXPECT_FALSE(encodeManifest(snapshot(), budget));
    }
    QBuffer buffer; buffer.setData(bytes.value()); ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    auto result = readManifest(buffer, {100, 4096, 32});
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted); EXPECT_EQ(buffer.pos(), 0);
    result = readManifest(buffer, limits); ASSERT_TRUE(result);
    QByteArray deep(100, '['); deep += QByteArray(100, ']');
    result = decodeManifest(deep, limits);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
    QByteArray wide = "[";
    for (int i = 0; i < 5000; ++i) wide += "0,";
    wide += "0]";
    result = decodeManifest(wide, limits);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
}

TEST(Manifest, MetadataProjectionDoesNotTouchDatasetAndOmitsRuntimeState)
{
    const auto s = snapshot();
    ImportedDataset d;
    d.cacheId = s.pointCacheId; d.sourcePath = s.source.path.toStdWString();
    d.sourceBytes = s.source.bytes; d.sourceLines = s.source.lines;
    d.pointCount = 3; d.minimum = {-0.0, -1e200}; d.maximum = {1e250, std::numeric_limits<double>::denorm_min()};
    d.hasNonZeroZ = true;
    d.location = std::make_shared<const DatasetLocation>(DatasetLocation{"old-storage", L"Z:/absent", L"missing.cache"});
    ProjectState state(19, s.projectId, d); state.changeCell(s.cell);
    const auto projected = captureSnapshot(state, s.storageId, s.snapshotId);
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.value().projectId, s.projectId);
    EXPECT_EQ(projected.value().datasets[0].relativePath, s.datasets[0].relativePath);
    const auto encoded = encodeManifest(projected.value(), limits);
    ASSERT_TRUE(encoded);
    EXPECT_FALSE(encoded.value().contains("old-storage")); EXPECT_FALSE(encoded.value().contains("Z:"));
    EXPECT_FALSE(encoded.value().contains("Revision")); EXPECT_FALSE(encoded.value().contains("runtime"));
    EXPECT_TRUE(decodeManifest(encoded.value(), limits));
    EXPECT_FALSE(captureSnapshot(ProjectState(1), s.storageId, s.snapshotId));
    EXPECT_THROW(ProjectState(1, ProjectId{}, d), std::invalid_argument);
}

TEST(Manifest, FileDeviceContractAndReadFailures)
{
    const auto encoded = encodeManifest(snapshot(), limits);
    ASSERT_TRUE(encoded);
    QBuffer buffer;
    EXPECT_FALSE(readManifest(buffer, limits));
    buffer.setData(encoded.value()); ASSERT_TRUE(buffer.open(QIODevice::ReadOnly));
    ASSERT_TRUE(buffer.seek(1)); EXPECT_FALSE(readManifest(buffer, limits));
    ASSERT_TRUE(buffer.seek(0)); buffer.setTextModeEnabled(true); EXPECT_FALSE(readManifest(buffer, limits));
    buffer.setTextModeEnabled(false);
    class BrokenBuffer : public QBuffer {
    public:
        bool sequential = false;
        bool isSequential() const override { return sequential; }
    protected:
        qint64 readData(char *, qint64) override { return -1; }
    } broken;
    broken.setData(encoded.value()); ASSERT_TRUE(broken.open(QIODevice::ReadOnly));
    auto failure = readManifest(broken, limits);
    ASSERT_FALSE(failure); EXPECT_EQ(failure.error().code, ErrorCode::FileIO);
    broken.sequential = true;
    failure = readManifest(broken, limits);
    ASSERT_FALSE(failure); EXPECT_EQ(failure.error().code, ErrorCode::FileIO);
}

TEST(Manifest, DecodingRejectsUnsafePathsAndReversedBounds)
{
    for (auto path : {"../outside", "C:/outside", "data/wrong-cache.pc2dcache"}) {
        auto o = validJson(); auto d = o["datasets"].toArray()[0].toObject();
        d["path"] = path; o["datasets"] = QJsonArray{d}; expectError(o, ErrorCode::InvalidReference);
    }
    auto o = validJson(); auto d = o["datasets"].toArray()[0].toObject();
    auto b = d["bounds"].toObject(); b["minX"] = "2"; b["maxX"] = "1";
    d["bounds"] = b; o["datasets"] = QJsonArray{d}; expectError(o, ErrorCode::InvalidValue);
}

TEST(StoragePath, PortableUnicodeAndMissingDatasetAndDirectoryMove)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid());
    const auto a = area.path() + "/before"; const auto b = area.path() + "/после";
    ASSERT_TRUE(QDir().mkpath(a));
    const QString relative = QStringLiteral("data/точки/Деталь 1.pc2dcache");
    const auto missing = resolveDatasetPath(a, relative);
    ASSERT_TRUE(missing); EXPECT_FALSE(missing.value().exists);
    write(a + '/' + relative);
    const auto present = resolveDatasetPath(a, relative);
    ASSERT_TRUE(present); EXPECT_TRUE(present.value().exists);
    ASSERT_TRUE(QDir().rename(a, b));
    const auto moved = resolveDatasetPath(b, relative);
    ASSERT_TRUE(moved); EXPECT_TRUE(moved.value().exists);
    EXPECT_NE(moved.value().absolutePath, present.value().absolutePath);
    EXPECT_TRUE(QFileInfo::exists(moved.value().absolutePath));
}

TEST(StoragePath, RejectsWindowsAmbiguityTraversalDevicesAndAds)
{
    for (const auto &path : {"", "/absolute", "C:/absolute", "C:relative", "//server/share/x", "\\\\server\\share\\x",
        "\\\\?\\C:\\x", "a\\b", "../x", "a/../x", "a/./b", "a//b", "a/", "a/.",
        "a/x.", "a/x ", "a/a:b", "a/x:stream", "a/*", "a/?", "a/\"", "a/<", "a/>", "a/|",
        "a/CON", "a/con.txt", "a/PRN", "a/AUX", "a/NUL", "a/COM1.txt", "a/lpt9", "a/CONIN$", "a/CONOUT$"}) {
        SCOPED_TRACE(path); EXPECT_FALSE(validateDatasetPath(QString::fromUtf8(path)));
    }
    for (auto name : {QStringLiteral("a/COM¹.txt"), QStringLiteral("a/LPT²"), QStringLiteral("a/COM³")})
        EXPECT_FALSE(validateDatasetPath(name));
    EXPECT_FALSE(validateDatasetPath(QString("a/") + QChar(0)));
    EXPECT_FALSE(validateDatasetPath(QString("a/") + QChar(0xd800)));
    EXPECT_FALSE(validateDatasetPath(QString("a/") + QChar(31)));
    EXPECT_FALSE(validateDatasetPath(QString(256, u'a')));
    EXPECT_TRUE(validateDatasetPath("data/COM10.pc2dcache"));
}

TEST(StoragePath, RejectsDirectoryAsFileAndFileAsParent)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid());
    ASSERT_TRUE(QDir(area.path()).mkdir("folder"));
    EXPECT_FALSE(resolveDatasetPath(area.path(), "folder"));
    write(area.path() + "/file");
    EXPECT_FALSE(resolveDatasetPath(area.path(), "file/child.cache"));
    EXPECT_FALSE(resolveDatasetPath("relative", "file"));
    EXPECT_FALSE(resolveDatasetPath(area.path() + "/absent", "file"));
}

TEST(StoragePath, JunctionEscapeIncludingMissingLeaf)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid());
    const auto project = area.path() + "/project"; const auto outside = area.path() + "/outside";
    ASSERT_TRUE(QDir().mkpath(project)); ASSERT_TRUE(QDir().mkpath(outside));
    write(outside + "/file.cache");
    QProcess command;
    command.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(project + "/link"), QDir::toNativeSeparators(outside)});
    ASSERT_TRUE(command.waitForFinished());
    ASSERT_EQ(command.exitCode(), 0) << command.readAllStandardError().toStdString();
    EXPECT_FALSE(resolveDatasetPath(project, "link/file.cache"));
    EXPECT_FALSE(resolveDatasetPath(project, "link/missing/deep.cache"));
    // Remove only the junction itself; never traverse/delete its target.
    ASSERT_TRUE(RemoveDirectoryW((project + "/link").toStdWString().c_str()));
    EXPECT_TRUE(QFileInfo::exists(outside + "/file.cache"));
}

TEST(StoragePath, FileSymlinkEscape)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid());
    const auto project = area.path() + "/project"; ASSERT_TRUE(QDir().mkpath(project));
    const auto outside = area.path() + "/outside.cache"; write(outside);
    const auto link = project + "/link.cache";
    if (!CreateSymbolicLinkW(link.toStdWString().c_str(), outside.toStdWString().c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const auto error = GetLastError();
        if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER)
            GTEST_SKIP() << "Windows file symlink privilege unavailable: " << error;
        FAIL() << "CreateSymbolicLinkW failed: " << error;
    }
    EXPECT_FALSE(resolveDatasetPath(project, "link.cache"));
    ASSERT_TRUE(DeleteFileW(link.toStdWString().c_str()));
    EXPECT_TRUE(QFileInfo::exists(outside));
}
