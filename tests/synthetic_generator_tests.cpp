#include "generator.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace synthetic;

namespace {

QString fixture(const char *kind, const std::string &name)
{
    return QStringLiteral(SYNTHETIC_FIXTURES) + "/" + kind + "/" + QString::fromStdString(name) + ".json";
}

QByteArray bytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    return file.readAll();
}

PartGeometry part(const std::string &name) { return parseGeometry(bytes(fixture("geometry", name))); }
ScanScenario scan(const std::string &name) { return parseScan(bytes(fixture("scans", name))); }

void intervals(const PartGeometry &g, Direction d, double v, std::initializer_list<Interval> expected)
{
    const auto actual = materialIntervals(g, d, v);
    ASSERT_EQ(actual.size(), expected.size());
    std::size_t i = 0;
    for (const auto wanted : expected) {
        EXPECT_NEAR(actual[i].min, wanted.min, 1e-10);
        EXPECT_NEAR(actual[i].max, wanted.max, 1e-10);
        ++i;
    }
}

std::vector<double> samples(Interval interval, double max = 200, double step = 0.07)
{
    std::vector<double> result;
    sampleInterval(interval, max, step, [&](double u) { result.push_back(u); });
    return result;
}

Contour rectangle(double x0, double y0, double x1, double y1)
{
    return std::vector<Segment>{Line{{x0, y0}, {x1, y0}}, Line{{x1, y0}, {x1, y1}},
                                Line{{x1, y1}, {x0, y1}}, Line{{x0, y1}, {x0, y0}}};
}

void expectInvalid(const PartGeometry &g, const std::string &message)
{
    try {
        validateGeometry(g);
        FAIL() << "Expected geometry rejection";
    } catch (const Error &error) {
        EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
    }
}

const std::vector<std::string> geometryNames{
    "rectangle", "circle", "triangle", "rectangle_one_circle", "rectangle_multiple_circles",
    "rectangle_cutout", "line_arc_part", "complex_part"};

class GeometryFixture : public testing::TestWithParam<std::string> {};
TEST_P(GeometryFixture, ReadsAndValidates)
{
    const auto g = part(GetParam());
    EXPECT_NO_THROW(validateGeometry(g));
    const auto b = geometryBounds(g);
    EXPECT_GT(b.max.x, b.min.x);
    EXPECT_GT(b.max.y, b.min.y);
}
INSTANTIATE_TEST_SUITE_P(AllGeometry, GeometryFixture, testing::ValuesIn(geometryNames),
                        [](const auto &info) { return info.param; });

TEST(SyntheticJson, SyntaxFieldsTypesAndEnums)
{
    EXPECT_THROW(parseGeometry("{"), Error);
    EXPECT_THROW(parseGeometry("[]"), Error);
    auto g = QJsonDocument::fromJson(bytes(fixture("geometry", "rectangle"))).object();
    g["units"] = "inch";
    EXPECT_THROW(parseGeometry(QJsonDocument(g).toJson()), Error);
    g["units"] = "mm";
    g["unknown"] = 1;
    EXPECT_THROW(parseGeometry(QJsonDocument(g).toJson()), Error);
    auto input = bytes(fixture("geometry", "rectangle"));
    input.replace("\"LINE\"", "\"SPLINE\"");
    EXPECT_THROW(parseGeometry(input), Error);
    auto s = QJsonDocument::fromJson(bytes(fixture("scans", "clean_horizontal"))).object();
    for (const char *field : {"pointStep", "lineStep", "maxLineLength"}) {
        auto bad = s; bad[field] = 0;
        EXPECT_THROW(parseScan(QJsonDocument(bad).toJson()), Error);
        bad[field] = -1;
        EXPECT_THROW(parseScan(QJsonDocument(bad).toJson()), Error);
        bad[field] = "0.07";
        EXPECT_THROW(parseScan(QJsonDocument(bad).toJson()), Error);
    }
    for (double seed : {-1.0, 1.5, 4294967296.0}) {
        auto bad = s; bad["seed"] = seed;
        EXPECT_THROW(parseScan(QJsonDocument(bad).toJson()), Error);
    }
    s["direction"] = "Diagonal";
    EXPECT_THROW(parseScan(QJsonDocument(s).toJson()), Error);
    EXPECT_THROW(parseScan("{\"pointStep\":1e400}"), Error);
}

TEST(SyntheticJson, InvalidArcRadiusAndMissingFields)
{
    auto input = bytes(fixture("geometry", "line_arc_part"));
    auto bad = input; bad.replace("-3.141592653589793", "0");
    EXPECT_THROW(parseGeometry(bad), Error);
    bad = input; bad.replace("-3.141592653589793", "6.283185307179586");
    EXPECT_THROW(parseGeometry(bad), Error);
    bad = input; bad.replace("\"radius\": 20", "\"radius\": -20");
    EXPECT_THROW(parseGeometry(bad), Error);
    EXPECT_THROW(parseGeometry("{\"formatVersion\":1,\"units\":\"mm\"}"), Error);
    EXPECT_THROW(parseGeometry("{\"formatVersion\":1,\"units\":\"mm\",\"outerContour\":"
                               "{\"type\":\"CIRCLE\",\"center\":[0,0],\"radius\":0},\"innerContours\":[]}"), Error);
}

TEST(SyntheticJson, AllAdvancedFixturesAreExplicitlyRejected)
{
    for (const std::string name : {"shifted_passes", "overlapping_passes", "double_scan", "outside_grid_cloud",
                                   "jagged_boundary", "extra_table_fragment", "mixed_artifacts"}) {
        try {
            scan(name);
            FAIL() << name;
        } catch (const Error &error) {
            EXPECT_NE(std::string(error.what()).find("unsupported in current generator implementation"),
                      std::string::npos) << name << ": " << error.what();
        }
    }
}

TEST(SyntheticValidation, OpenZeroAndSelfIntersectingChains)
{
    auto g = part("rectangle");
    std::get<Line>(std::get<std::vector<Segment>>(g.outer)[0]).end.x = 99;
    expectInvalid(g, "not closed");
    g = part("rectangle");
    auto &l = std::get<Line>(std::get<std::vector<Segment>>(g.outer)[0]); l.end = l.start;
    expectInvalid(g, "zero");
    g.outer = std::vector<Segment>{Line{{0, 0}, {10, 10}}, Line{{10, 10}, {0, 10}},
                                   Line{{0, 10}, {10, 0}}, Line{{10, 0}, {0, 0}}};
    expectInvalid(g, "self-intersection");
    g.outer = std::vector<Segment>{Line{{0, 0}, {10, 0}}, Line{{10, 0}, {0, 0}}};
    expectInvalid(g, "overlapping");
}

TEST(SyntheticValidation, OutsideIntersectingTouchingAndNestedInner)
{
    auto g = part("rectangle");
    g.inner = {Circle{{150, 30}, 5}}; expectInvalid(g, "not strictly inside");
    g.inner = {Circle{{99, 30}, 5}}; expectInvalid(g, "intersect or touch");
    g.inner = {Circle{{95, 30}, 5}}; expectInvalid(g, "intersect or touch");
    g.inner = {Circle{{45, 30}, 5}, Circle{{55, 30}, 5}}; expectInvalid(g, "intersect or touch");
    g.inner = {Circle{{50, 30}, 10}, Circle{{50, 30}, 5}}; expectInvalid(g, "nested");
    g.inner = {rectangle(20, 20, 40, 40), rectangle(30, 25, 50, 45)};
    expectInvalid(g, "intersect or touch");
}

TEST(SyntheticGeometry, RectangleBothDirectionsAndBoundaryLines)
{
    const auto g = part("rectangle");
    for (double y : {0.0, 30.0, 60.0})
        intervals(g, Direction::Horizontal, y, {{0, 100}});
    for (double x : {0.0, 50.0, 100.0})
        intervals(g, Direction::Vertical, x, {{0, 60}});
    intervals(g, Direction::Horizontal, -1, {});
    for (auto direction : {Direction::Horizontal, Direction::Vertical}) {
        auto s = scan(direction == Direction::Horizontal ? "clean_horizontal" : "clean_vertical");
        std::int64_t count = 0;
        Bounds bounds{{1000, 1000}, {-1000, -1000}};
        Point previous{-1000, -1000};
        generate(g, s, [&](Point p) {
            bounds.min.x = std::min(bounds.min.x, p.x); bounds.min.y = std::min(bounds.min.y, p.y);
            bounds.max.x = std::max(bounds.max.x, p.x); bounds.max.y = std::max(bounds.max.y, p.y);
            if ((direction == Direction::Horizontal ? p.y == previous.y : p.x == previous.x)) {
                const double step = direction == Direction::Horizontal ? p.x - previous.x : p.y - previous.y;
                EXPECT_GT(step, 0); EXPECT_LE(step, s.pointStep + 1e-12);
            }
            previous = p; ++count;
        });
        EXPECT_EQ(count, direction == Direction::Horizontal ? 87230 : 86759);
        EXPECT_DOUBLE_EQ(bounds.min.x, 0); EXPECT_DOUBLE_EQ(bounds.min.y, 0);
        EXPECT_DOUBLE_EQ(bounds.max.x, 100); EXPECT_DOUBLE_EQ(bounds.max.y, 60);
    }
}

TEST(SyntheticGeometry, CircleChordsTangenciesAndContainment)
{
    const auto g = part("circle");
    intervals(g, Direction::Horizontal, 50, {{0, 100}});
    intervals(g, Direction::Horizontal, 20, {{10, 90}});
    for (auto d : {Direction::Horizontal, Direction::Vertical}) {
        intervals(g, d, 0, {{50, 50}});
        intervals(g, d, 100, {{50, 50}});
        EXPECT_EQ(samples(materialIntervals(g, d, 0).front()).size(), 1);
        auto s = scan("clean_horizontal"); s.direction = d;
        generate(g, s, [](Point p) { EXPECT_LE(std::hypot(p.x - 50, p.y - 50), 50 + 1e-12); });
    }
}

TEST(SyntheticGeometry, TriangleShortIntervalAndExactVertex)
{
    const auto g = part("triangle");
    const auto i = materialIntervals(g, Direction::Horizontal, 60);
    ASSERT_EQ(i.size(), 1);
    EXPECT_NEAR(i[0].max - i[0].min, 100 * (1 - 60 / 60.01), 1e-12);
    const auto p = samples(i[0]); ASSERT_EQ(p.size(), 2);
    EXPECT_DOUBLE_EQ(p.front(), i[0].min); EXPECT_DOUBLE_EQ(p.back(), i[0].max);
    intervals(g, Direction::Horizontal, 60.01, {{50, 50}});
    EXPECT_EQ(samples(materialIntervals(g, Direction::Horizontal, 60.01).front()).size(), 1);
}

TEST(SyntheticGeometry, OneCircleStrictInteriorAndTangency)
{
    const auto g = part("rectangle_one_circle");
    intervals(g, Direction::Horizontal, 30, {{0, 40}, {60, 100}});
    for (double y : {20.0, 40.0})
        intervals(g, Direction::Horizontal, y, {{0, 100}});
    generate(g, scan("clean_horizontal"), [](Point p) {
        EXPECT_GE(std::hypot(p.x - 50, p.y - 30), 10 - 1e-12);
    });
}

TEST(SyntheticGeometry, MultipleCircles)
{
    const auto g = part("rectangle_multiple_circles");
    intervals(g, Direction::Horizontal, 25, {{0, 20}, {30, 54.8}, {65.2, 85}, {105, 120}});
    intervals(g, Direction::Horizontal, 60, {{0, 33}, {47, 77.8}, {92.2, 120}});
}

TEST(SyntheticGeometry, CutoutBoundaryRunsRemainMaterial)
{
    const auto g = part("rectangle_cutout");
    intervals(g, Direction::Horizontal, 30, {{0, 35}, {65, 100}});
    for (double y : {20.0, 40.0}) intervals(g, Direction::Horizontal, y, {{0, 100}});
    for (double x : {35.0, 65.0}) intervals(g, Direction::Vertical, x, {{0, 60}});
    intervals(g, Direction::Vertical, 50, {{0, 20}, {40, 60}});
}

TEST(SyntheticGeometry, ClockwiseArcAcrossZeroAndReversedContour)
{
    auto g = part("line_arc_part");
    intervals(g, Direction::Horizontal, 20, {{0, 100}});
    intervals(g, Direction::Horizontal, 0, {{0, 80}});
    intervals(g, Direction::Horizontal, 40, {{0, 80}});
    intervals(g, Direction::Vertical, 90, {{20 - std::sqrt(300), 20 + std::sqrt(300)}});
    auto &segments = std::get<std::vector<Segment>>(g.outer);
    std::reverse(segments.begin(), segments.end());
    for (auto &s : segments) {
        if (auto *l = std::get_if<Line>(&s)) std::swap(l->start, l->end);
        else { auto &a = std::get<Arc>(s); a.startAngle += a.sweepAngle; a.sweepAngle = -a.sweepAngle; }
    }
    ASSERT_NO_THROW(validateGeometry(g));
    intervals(g, Direction::Horizontal, 20, {{0, 100}});
    intervals(g, Direction::Vertical, 90, {{20 - std::sqrt(300), 20 + std::sqrt(300)}});
}

TEST(SyntheticGeometry, ComplexPartLinesAndBounds)
{
    const auto g = part("complex_part");
    intervals(g, Direction::Horizontal, 0, {{10, 130}});
    intervals(g, Direction::Horizontal, 45, {{0, 52}, {88, 102}, {118, 140}});
    intervals(g, Direction::Vertical, 0, {{10, 80}});
    const auto b = geometryBounds(g);
    EXPECT_DOUBLE_EQ(b.min.x, 0); EXPECT_DOUBLE_EQ(b.min.y, 0);
    EXPECT_DOUBLE_EQ(b.max.x, 140); EXPECT_DOUBLE_EQ(b.max.y, 90);
}

TEST(SyntheticSampling, SegmentationSeamsAndShortTails)
{
    const auto p = samples({0, 100}, 25);
    ASSERT_EQ(p.size(), 1433);
    for (double u : {0.0, 25.0, 50.0, 75.0, 100.0})
        EXPECT_EQ(std::count(p.begin(), p.end(), u), 1);
    const auto tail = samples({0, 25.01}, 25);
    EXPECT_DOUBLE_EQ(tail.back(), 25.01);
    EXPECT_DOUBLE_EQ(tail[tail.size() - 2], 25);
    const auto tiny = samples({0, 25.000000001}, 25);
    EXPECT_DOUBLE_EQ(tiny.back(), 25.000000001);
    const auto small = samples({1, 1 + 1e-12}); ASSERT_EQ(small.size(), 2);
    for (std::size_t i = 1; i < p.size(); ++i)
        EXPECT_LE(p[i] - p[i - 1], 0.07 + 1e-12);
}

TEST(SyntheticSampling, OverflowAndUnrepresentableStepsFail)
{
    EXPECT_THROW(samples({0, 100}, 25, 1e-300), Error);
    auto s = scan("clean_horizontal"); s.lineStep = 1e-300;
    EXPECT_THROW(generate(part("rectangle"), s, [](Point) {}), Error);
    EXPECT_THROW(samples({1e15, 1e15 + 1}, 1, 0.01), Error);
}

TEST(SyntheticSampling, DecimalSeamsAndUnderflowDoNotRemovePositiveLengths)
{
    const auto p = samples({0, 1}, 0.1, 0.03);
    EXPECT_DOUBLE_EQ(p.front(), 0); EXPECT_DOUBLE_EQ(p.back(), 1);
    for (int k = 1; k < 10; ++k)
        EXPECT_EQ(std::count(p.begin(), p.end(), k * 0.1), 1);
    const auto tiny = samples({0, 1e-200}, 1e200, 1e200);
    ASSERT_EQ(tiny.size(), 2);
    EXPECT_DOUBLE_EQ(tiny.back(), 1e-200);
}

TEST(SyntheticGeometry, CloseIndependentIntersectionsRemainDistinct)
{
    PartGeometry g{rectangle(0, 0, 1, 1), {rectangle(0.5, 0.2, 0.5 + 1e-12, 0.8)}};
    ASSERT_NO_THROW(validateGeometry(g));
    const auto i = materialIntervals(g, Direction::Horizontal, 0.5);
    ASSERT_EQ(i.size(), 2);
    EXPECT_DOUBLE_EQ(i[0].max, 0.5); EXPECT_DOUBLE_EQ(i[1].min, 0.5 + 1e-12);
    EXPECT_GT(i[1].min, i[0].max);
}

TEST(SyntheticGeometry, ArcJunctionAtOriginUsesLocalPrimitiveScale)
{
    PartGeometry g{std::vector<Segment>{Line{{0, 0}, {1, 0}}, Line{{1, 0}, {1, 1}},
                                        Arc{{1, 0}, 1, std::numbers::pi / 2, std::numbers::pi / 2}}, {}};
    ASSERT_NO_THROW(validateGeometry(g));
    intervals(g, Direction::Horizontal, 0, {{0, 1}});
    intervals(g, Direction::Horizontal, 1, {{1, 1}});
    intervals(g, Direction::Horizontal, 0.5, {{1 - std::sqrt(0.75), 1}});
}

TEST(SyntheticGeometry, AdjacentArcsAndAlmostFullSingleArc)
{
    const double pi = std::numbers::pi;
    PartGeometry g{std::vector<Segment>{Arc{{0, 0}, 1, 0, pi}, Arc{{0, 0}, 1, pi, pi}}, {}};
    ASSERT_NO_THROW(validateGeometry(g));
    intervals(g, Direction::Horizontal, 0, {{-1, 1}});
    intervals(g, Direction::Horizontal, 1, {{0, 0}});
    g.outer = std::vector<Segment>{Arc{{0, 0}, 1, 0, std::nextafter(2 * pi, 0.0)}};
    expectInvalid(g, "cannot form a closed contour");
}

struct RunResult { int exitCode; QByteArray out; QByteArray error; };
RunResult run(const QStringList &args)
{
    QProcess process;
    process.start(QStringLiteral(SYNTHETIC_GENERATOR_EXE), args);
    if (!process.waitForStarted() || !process.waitForFinished(120000))
        throw std::runtime_error(process.errorString().toStdString());
    if (process.exitStatus() != QProcess::NormalExit)
        throw std::runtime_error("generator crashed");
    return {process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
}

QStringList args(const std::string &geometry, const std::string &scenario, const QString &output)
{
    return {"--geometry", fixture("geometry", geometry), "--scan", fixture("scans", scenario), "--output", output};
}

QByteArray sha256(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex(); }

void checkDataset(const QString &output, const std::string &geometry, const std::string &scenario)
{
    const auto content = bytes(output);
    const auto m = QJsonDocument::fromJson(bytes(output + ".manifest.json")).object();
    EXPECT_EQ(m["manifestVersion"].toInt(), 1);
    EXPECT_EQ(m["generatorVersion"].toString(), generatorVersion);
    EXPECT_EQ(m["units"].toString(), "mm");
    EXPECT_EQ(m["outputSha256"].toString().toLatin1(), sha256(content));
    EXPECT_EQ(m["geometry"].toObject()["sha256"].toString().toLatin1(), sha256(bytes(fixture("geometry", geometry))));
    EXPECT_EQ(m["scan"].toObject()["sha256"].toString().toLatin1(), sha256(bytes(fixture("scans", scenario))));
    const auto s = scan(scenario);
    EXPECT_EQ(m["seed"].toInteger(), s.seed);
    EXPECT_DOUBLE_EQ(m["pointStep"].toDouble(), s.pointStep);
    EXPECT_DOUBLE_EQ(m["lineStep"].toDouble(), s.lineStep);
    EXPECT_DOUBLE_EQ(m["maxLineLength"].toDouble(), s.maxLineLength);
    EXPECT_EQ(m["direction"].toString(), s.direction == Direction::Horizontal ? "Horizontal" : "Vertical");
    EXPECT_EQ(m["outputFormat"].toString(), QFileInfo(output).suffix());
    EXPECT_FALSE(content.contains('\r'));
    EXPECT_FALSE(content.contains("-0.000000"));
    std::int64_t count = 0;
    Bounds b{{1e100, 1e100}, {-1e100, -1e100}};
    const char *cursor = content.constData(), *end = cursor + content.size();
    while (cursor != end) {
        double values[2];
        for (auto &v : values) {
            const char *begin = cursor;
            const auto parsed = std::from_chars(cursor, end, v);
            ASSERT_EQ(parsed.ec, std::errc{});
            cursor = parsed.ptr;
            ASSERT_LT(cursor, end); ASSERT_EQ(*cursor++, ' ');
            const auto token = std::string_view(begin, static_cast<std::size_t>(cursor - begin - 1));
            const auto decimal = token.find('.');
            ASSERT_NE(decimal, std::string_view::npos); ASSERT_EQ(token.size() - decimal - 1, 6);
        }
        ASSERT_GE(end - cursor, 2); ASSERT_EQ(*cursor++, '0'); ASSERT_EQ(*cursor++, '\n');
        b.min.x = std::min(b.min.x, values[0]); b.min.y = std::min(b.min.y, values[1]);
        b.max.x = std::max(b.max.x, values[0]); b.max.y = std::max(b.max.y, values[1]);
        ++count;
    }
    EXPECT_EQ(m["pointCount"].toInteger(), count);
    const auto box = m["boundingBox"].toObject();
    EXPECT_DOUBLE_EQ(box["min"].toArray()[0].toDouble(), b.min.x);
    EXPECT_DOUBLE_EQ(box["min"].toArray()[1].toDouble(), b.min.y);
    EXPECT_DOUBLE_EQ(box["max"].toArray()[0].toDouble(), b.max.x);
    EXPECT_DOUBLE_EQ(box["max"].toArray()[1].toDouble(), b.max.y);
    EXPECT_EQ(box["min"].toArray()[2].toInt(), 0); EXPECT_EQ(box["max"].toArray()[2].toInt(), 0);
    if (geometry == "rectangle")
        EXPECT_EQ(count, scenario == "clean_horizontal" ? 87230 : scenario == "clean_vertical" ? 86759 : 87413);
}

class DatasetFixture : public testing::TestWithParam<std::tuple<std::string, std::string>> {};
TEST_P(DatasetFixture, CliOutputAndManifest)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto &[g, s] = GetParam();
    const QString output = directory.filePath(s == "clean_vertical" ? "part.asc" : "part.xyz");
    const auto result = run(args(g, s, output));
    ASSERT_EQ(result.exitCode, 0) << result.error.toStdString();
    EXPECT_TRUE(result.error.isEmpty()); EXPECT_FALSE(result.out.isEmpty());
    checkDataset(output, g, s);
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files).size(), 2);
}
INSTANTIATE_TEST_SUITE_P(BasicMatrix, DatasetFixture,
                        testing::Combine(testing::ValuesIn(geometryNames),
                                         testing::Values("clean_horizontal", "clean_vertical", "short_segments_horizontal")),
                        [](const auto &info) { return std::get<0>(info.param) + "_" + std::get<1>(info.param); });

TEST(SyntheticIntegration, IndependentRunsAreByteIdentical)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto output = directory.filePath("part.xyz");
    ASSERT_EQ(run(args("rectangle", "clean_horizontal", output)).exitCode, 0);
    const auto first = bytes(output), manifest = bytes(output + ".manifest.json");
    ASSERT_TRUE(QFile::remove(output)); ASSERT_TRUE(QFile::remove(output + ".manifest.json"));
    ASSERT_EQ(run(args("rectangle", "clean_horizontal", output)).exitCode, 0);
    EXPECT_EQ(first, bytes(output)); EXPECT_EQ(manifest, bytes(output + ".manifest.json"));
}

TEST(SyntheticIntegration, CliErrorsAndExistingTargetsRemainUntouched)
{
    EXPECT_EQ(run({"--help"}).exitCode, 0); EXPECT_EQ(run({"--version"}).out.trimmed(), generatorVersion);
    EXPECT_NE(run({}).exitCode, 0); EXPECT_NE(run({"--unknown", "x"}).exitCode, 0);
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto output = directory.filePath("part.xyz");
    ASSERT_EQ(run(args("rectangle", "clean_horizontal", output)).exitCode, 0);
    const auto first = bytes(output), manifest = bytes(output + ".manifest.json");
    auto result = run(args("rectangle", "clean_horizontal", output));
    EXPECT_NE(result.exitCode, 0); EXPECT_TRUE(result.out.isEmpty()); EXPECT_TRUE(result.error.contains("already exists"));
    EXPECT_EQ(first, bytes(output)); EXPECT_EQ(manifest, bytes(output + ".manifest.json"));
    ASSERT_TRUE(QFile::remove(output));
    EXPECT_NE(run(args("rectangle", "clean_horizontal", output)).exitCode, 0);
    EXPECT_EQ(manifest, bytes(output + ".manifest.json"));
    result = run(args("rectangle", "double_scan", directory.filePath("advanced.xyz")));
    EXPECT_NE(result.exitCode, 0); EXPECT_TRUE(result.out.isEmpty()); EXPECT_TRUE(result.error.contains("unsupported"));
    EXPECT_FALSE(QFile::exists(directory.filePath("advanced.xyz")));
    EXPECT_NE(run(args("rectangle", "clean_horizontal", directory.filePath("missing/part.xyz"))).exitCode, 0);
    auto missing = args("rectangle", "clean_horizontal", directory.filePath("missing.xyz")); missing[1] = "absent.json";
    EXPECT_NE(run(missing).exitCode, 0);
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files).size(), 1);
}

TEST(SyntheticIntegration, PrecisionCollapseFailsAndRemovesTemporaryFiles)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    auto g = bytes(fixture("geometry", "rectangle")); g.replace("100", "0.0000001");
    QFile file(directory.filePath("tiny.json")); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(g), g.size()); file.close();
    auto a = args("rectangle", "clean_horizontal", directory.filePath("tiny.xyz")); a[1] = file.fileName();
    const auto result = run(a);
    EXPECT_NE(result.exitCode, 0); EXPECT_TRUE(result.error.contains("precision collapses")) << result.error.toStdString();
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files), QStringList{"tiny.json"});
}

} // namespace
