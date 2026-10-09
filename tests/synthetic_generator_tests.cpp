#include "generator.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
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

std::vector<Point> cloud(const PartGeometry &g, const ScanScenario &s)
{
    std::vector<Point> points;
    generate(g, s, [&](Point p) { points.push_back(p); });
    return points;
}

void equalClouds(const std::vector<Point> &a, const std::vector<Point> &b)
{
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_DOUBLE_EQ(a[i].x, b[i].x) << i;
        ASSERT_DOUBLE_EQ(a[i].y, b[i].y) << i;
    }
}

bool hasPoint(const std::vector<Point> &points, double x, double y)
{
    return std::any_of(points.begin(), points.end(), [=](Point p) {
        return std::abs(p.x - x) < 1e-10 && std::abs(p.y - y) < 1e-10;
    });
}

TEST(SyntheticArtifacts, RegionClipsCoverageWithoutMovingGridOrigin)
{
    PartGeometry g{rectangle(0, 0, 10, 10), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 1; s.lineStep = 2; s.maxLineLength = 4;
    s.passes = {{Direction::Horizontal, Bounds{{2.5, 2.2}, {7.5, 7.8}}, 0.5, {}, {}}};
    const auto points = cloud(g, s);
    ASSERT_EQ(points.size(), 18);
    for (double y : {2.5, 4.5, 6.5}) {
        EXPECT_TRUE(hasPoint(points, 2.5, y)); EXPECT_TRUE(hasPoint(points, 7.5, y));
    }
    for (Point p : points) {
        EXPECT_GE(p.x, 2.5); EXPECT_LE(p.x, 7.5);
        EXPECT_TRUE(p.y == 2.5 || p.y == 4.5 || p.y == 6.5);
    }
    s.passes[0].region = Bounds{{11, 11}, {12, 12}};
    EXPECT_TRUE(cloud(g, s).empty());
    s.passes[0].region = Bounds{{10, 0}, {11, 10}}; s.passes[0].lineOffset = 0;
    const auto contacts = cloud(g, s);
    ASSERT_EQ(contacts.size(), 6);
    for (Point p : contacts) EXPECT_DOUBLE_EQ(p.x, 10);
}

TEST(SyntheticArtifacts, ShiftsContinuationAndSeamsBothDirections)
{
    PartGeometry g{rectangle(0, 0, 10, 10), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 1; s.lineStep = 10; s.maxLineLength = 4;
    for (auto d : {Direction::Horizontal, Direction::Vertical}) {
        s.passes = {{d, std::nullopt, 0, {0.25, 0.5}, {-0.1, 0.2}}};
        const auto p = cloud(g, s);
        ASSERT_EQ(p.size(), 26);
        const auto check = [&](std::size_t i, double u, double v) {
            EXPECT_NEAR(d == Direction::Horizontal ? p[i].x : p[i].y, u, 1e-12);
            EXPECT_NEAR(d == Direction::Horizontal ? p[i].y : p[i].x, v, 1e-12);
        };
        check(0, 0.25, 0.5); check(4, 4.25, 0.5); check(5, 4.15, 0.7);
        check(9, 8.15, 0.7); check(10, 8.05, 0.9); check(12, 10.05, 0.9);
        check(13, 0.25, 10.5);
        s.passes[0].continuation = {};
        const auto joined = cloud(g, s);
        EXPECT_EQ(joined.size(), 22);
    }
}

TEST(SyntheticArtifacts, ContinuationIndexResetsForEachMaterialInterval)
{
    PartGeometry g{rectangle(0, 0, 10, 3), {Circle{{5, 1.5}, 1}}};
    auto s = scan("clean_horizontal"); s.pointStep = 1; s.maxLineLength = 2;
    s.passes = {{Direction::Horizontal, std::nullopt, 0.5, {0.25, 0}, {-0.1, 0.2}}};
    const auto points = cloud(g, s);
    EXPECT_TRUE(hasPoint(points, 0.25, 1.5)); EXPECT_TRUE(hasPoint(points, 6.25, 1.5));
    EXPECT_TRUE(hasPoint(points, 1.25, 1.5)); EXPECT_TRUE(hasPoint(points, 7.25, 1.5));
}

TEST(SyntheticArtifacts, OverlapAndDoubleScanPreserveEveryMeasurement)
{
    const auto g = part("rectangle");
    auto s = scan("overlapping_passes");
    const auto actual = cloud(g, s);
    std::vector<Point> expected;
    for (std::size_t i = 0; i < s.passes.size(); ++i)
        generatePass(g, s, i, [&](Point p) { expected.push_back(p); });
    equalClouds(actual, expected);
    EXPECT_EQ(actual.size(), 104071);
    EXPECT_TRUE(hasPoint(actual, 44.9, 0.5)); EXPECT_TRUE(hasPoint(actual, 99.9, 59.5));
    s = scan("clean_horizontal"); s.passes = {{s.direction, {}, 0, {}, {}}, {s.direction, {}, 0, {}, {}}};
    const auto twice = cloud(g, s);
    EXPECT_EQ(twice.size(), 2 * 87230);
    EXPECT_EQ(std::count_if(twice.begin(), twice.end(), [](Point p) { return p.x == 0 && p.y == 0; }), 2);
    s = scan("double_scan");
    const auto repeated = cloud(g, s);
    ASSERT_EQ(repeated.size(), 121590);
    for (std::size_t i = 87230; i < repeated.size(); ++i) {
        EXPECT_GE(repeated[i].x, 20.2); EXPECT_LE(repeated[i].x, 80.2);
        EXPECT_GE(repeated[i].y, 10.45 - 1e-12); EXPECT_LE(repeated[i].y, 49.45 + 1e-12);
    }
    EXPECT_TRUE(hasPoint(repeated, 20.2, 10.45)); EXPECT_TRUE(hasPoint(repeated, 80.2, 49.45));
}

TEST(SyntheticArtifacts, MissingRunsAreLocalShortAndKeepEndpointsAndLines)
{
    PartGeometry g{rectangle(0, 0, 20, 3), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 1; s.maxLineLength = 5;
    const auto clean = cloud(g, s);
    s.defects = {MissingPoints{Bounds{{2, 1}, {18, 2}}, 0.35, 1, 3}};
    const auto actual = cloud(g, s);
    EXPECT_LT(actual.size(), clean.size());
    for (int y = 0; y <= 3; ++y) {
        int run = 0;
        for (int x = 0; x <= 20; ++x) {
            if (hasPoint(actual, x, y)) run = 0;
            else {
                EXPECT_GE(x, 2); EXPECT_LE(x, 18); EXPECT_GE(y, 1); EXPECT_LE(y, 2);
                EXPECT_LE(++run, 3);
            }
            if (x % 5 == 0) EXPECT_TRUE(hasPoint(actual, x, y));
        }
    }
    equalClouds(actual, cloud(g, s));
    ++s.seed;
    const auto other = cloud(g, s);
    bool differs = actual.size() != other.size();
    if (!differs) for (std::size_t i = 0; i < actual.size(); ++i)
        differs = differs || actual[i].x != other[i].x || actual[i].y != other[i].y;
    EXPECT_TRUE(differs);
    std::get<MissingPoints>(s.defects[0]).probability = 0;
    equalClouds(clean, cloud(g, s));
    std::get<MissingPoints>(s.defects[0]).probability = 1;
    const auto dense = cloud(g, s);
    for (int y = 0; y <= 3; ++y) for (int x : {0, 5, 10, 15, 20}) EXPECT_TRUE(hasPoint(dense, x, y));
}

TEST(SyntheticArtifacts, RandomStreamsAreIndependentOfOtherDefectTypes)
{
    PartGeometry g{rectangle(0, 0, 20, 3), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 1;
    s.defects = {MissingPoints{std::nullopt, 0.3, 1, 3}};
    const auto expected = cloud(g, s);
    const auto seed = randomStreamSeed(s, 0, 0);
    s.defects.insert(s.defects.begin(), OutsideGridCloud{Bounds{{30, 0}, {31, 1}}, 1, 1});
    EXPECT_EQ(seed, randomStreamSeed(s, 0, 1));
    auto actual = cloud(g, s); ASSERT_EQ(actual.size(), expected.size() + 4); actual.resize(expected.size());
    equalClouds(expected, actual);
    EXPECT_NE(seed, randomStreamSeed(s, 1, 1));
}

TEST(SyntheticArtifacts, MissingRegionUsesMeasuredCoordinatesAndDisabledDefectPreservesSampling)
{
    PartGeometry g{rectangle(0, 0, 10, 2), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 1;
    s.passes = {{Direction::Horizontal, std::nullopt, 0, {20, 5}, {}}};
    s.defects = {MissingPoints{Bounds{{22, 6}, {28, 6.5}}, 1, 1, 1}};
    const auto p = cloud(g, s);
    EXPECT_FALSE(hasPoint(p, 22, 6)); EXPECT_TRUE(hasPoint(p, 23, 6));
    EXPECT_TRUE(hasPoint(p, 22, 5)); EXPECT_TRUE(hasPoint(p, 22, 7));
    for (const auto &name : geometryNames) for (const std::string scenario : {"clean_horizontal", "short_segments_horizontal"}) {
        auto clean = scan(scenario);
        const auto geometry = part(name);
        const auto expected = cloud(geometry, clean);
        clean.defects = {MissingPoints{std::nullopt, 0, 1, 3}};
        equalClouds(expected, cloud(geometry, clean));
    }
}

TEST(SyntheticArtifacts, StandaloneGridHasExactIndependentCoordinates)
{
    auto s = scan("outside_grid_cloud");
    std::vector<Point> points;
    generateStandalone(s, 0, [&](Point p) { points.push_back(p); });
    ASSERT_EQ(points.size(), 441);
    for (int j = 0; j <= 20; ++j) for (int i = 0; i <= 20; ++i) {
        EXPECT_DOUBLE_EQ(points[j * 21 + i].x, 155 + i * 0.5);
        EXPECT_DOUBLE_EQ(points[j * 21 + i].y, 15 + j * 0.5);
    }
    const auto g = part("complex_part");
    auto clean = s; clean.defects.clear();
    const auto prefix = cloud(g, clean), actual = cloud(g, s);
    ASSERT_EQ(actual.size(), prefix.size() + points.size());
    equalClouds(prefix, {actual.begin(), actual.begin() + prefix.size()});
    equalClouds(points, {actual.begin() + prefix.size(), actual.end()});
}

TEST(SyntheticArtifacts, TableFragmentUsesScanParametersWithoutPartClipping)
{
    auto s = scan("extra_table_fragment");
    for (auto d : {Direction::Horizontal, Direction::Vertical}) {
        s.direction = d;
        std::vector<Point> p;
        generateStandalone(s, 0, [&](Point point) { p.push_back(point); });
        const std::size_t perLine = d == Direction::Horizontal ? 430 : 859;
        ASSERT_EQ(p.size(), perLine * (d == Direction::Horizontal ? 61 : 31));
        EXPECT_DOUBLE_EQ(p.front().x, 155); EXPECT_DOUBLE_EQ(p.front().y, 5);
        EXPECT_DOUBLE_EQ(p.back().x, 185); EXPECT_DOUBLE_EQ(p.back().y, 65);
        for (std::size_t i = 1; i < p.size(); ++i) if (i % perLine != 0) {
            const auto delta = d == Direction::Horizontal ? p[i].x - p[i - 1].x : p[i].y - p[i - 1].y;
            EXPECT_GT(delta, 0); EXPECT_LE(delta, s.pointStep + 1e-12);
        }
    }
}

TEST(SyntheticArtifacts, JaggedOuterLineIsLocalAndSupportsBothDirections)
{
    PartGeometry g{rectangle(0, 0, 10, 10), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 1; s.maxLineLength = 4;
    for (auto d : {Direction::Horizontal, Direction::Vertical}) {
        s.direction = d;
        const Bounds region = d == Direction::Horizontal ? Bounds{{-1, 1}, {1, 3}} : Bounds{{1, -1}, {3, 1}};
        s.defects = {JaggedBoundary{region, false, 0, 1, 2, 0}};
        const auto p = cloud(g, s);
        EXPECT_TRUE(hasPoint(p, d == Direction::Horizontal ? -1 : 2, d == Direction::Horizontal ? 2 : -1));
        for (int v : {0, 1, 3, 4, 10}) EXPECT_TRUE(hasPoint(p, d == Direction::Horizontal ? 0 : v,
                                                                           d == Direction::Horizontal ? v : 0));
        // Continuation seams stay at their ideal locations and occur only once.
        EXPECT_EQ(std::count_if(p.begin(), p.end(), [d](Point point) {
            return d == Direction::Horizontal ? point.x == 4 && point.y == 2 : point.y == 4 && point.x == 2;
        }), 1);
        const auto first = p; ++s.seed; equalClouds(first, cloud(g, s));
        s.passes = {{d, Bounds{{2, 2}, {8, 8}}, 0, {}, {}}};
        const auto clipped = cloud(g, s);
        for (Point point : clipped) { EXPECT_GE(point.x, 2); EXPECT_GE(point.y, 2); }
        s.passes.clear();
    }
}

TEST(SyntheticArtifacts, JaggedCircleArcAndInnerBoundaries)
{
    auto s = scan("clean_horizontal");
    s.defects = {JaggedBoundary{Bounds{{-1, -1}, {101, 101}}, false, 0, 1, 2, 0}};
    for (auto d : {Direction::Horizontal, Direction::Vertical}) {
        s.direction = d;
        const auto p = cloud(part("circle"), s);
        EXPECT_TRUE(hasPoint(p, d == Direction::Horizontal ? -1 : 50, d == Direction::Horizontal ? 50 : -1));
        EXPECT_TRUE(hasPoint(p, d == Direction::Horizontal ? 101 : 50, d == Direction::Horizontal ? 50 : 101));
    }
    s.direction = Direction::Horizontal;
    s.defects = {JaggedBoundary{Bounds{{79, -1}, {101, 41}}, false, 0, 1, 2, 0}};
    EXPECT_TRUE(hasPoint(cloud(part("line_arc_part"), s), 101, 20));
    for (const auto &[name, boundary] : std::vector<std::pair<std::string, double>>{
             {"rectangle_one_circle", 40}, {"rectangle_cutout", 35}}) {
        s.defects = {JaggedBoundary{Bounds{{boundary - 1, 29}, {boundary + 1, 31}}, true, 0, 0.6, 2, 0}};
        const auto p = cloud(part(name), s);
        EXPECT_TRUE(hasPoint(p, boundary + 0.6, 30)); EXPECT_FALSE(hasPoint(p, boundary, 30));
    }
    s.direction = Direction::Vertical;
    s.defects = {JaggedBoundary{Bounds{{49, 19}, {51, 21}}, true, 0, 0.6, 2, 0}};
    EXPECT_TRUE(hasPoint(cloud(part("rectangle_one_circle"), s), 50, 20.6));
}

TEST(SyntheticArtifacts, JaggedRandomAmplitudeShortIntervalAndContacts)
{
    auto s = scan("jagged_boundary");
    const auto g = part("rectangle");
    const auto first = cloud(g, s); equalClouds(first, cloud(g, s));
    double previousRow = -1;
    for (Point p : first) {
        const bool endpoint = p.y != previousRow;
        previousRow = p.y;
        if (p.x >= 0) continue;
        EXPECT_GE(p.y, 15); EXPECT_LE(p.y, 35); EXPECT_GE(p.x, -0.75);
        const double phase = (p.y - 15) / 2;
        const double regular = 0.6 * (1 - std::abs(2 * (phase - std::floor(phase)) - 1));
        EXPECT_LE(-p.x, regular + 0.15 + 1e-12);
        if (endpoint) EXPECT_GE(-p.x, regular - 1e-12);
    }
    ++s.seed;
    const auto second = cloud(g, s);
    EXPECT_DOUBLE_EQ(first.front().x, second.front().x); // Outside the selected region.
    bool different = first.size() != second.size();
    for (std::size_t i = 0; i < std::min(first.size(), second.size()); ++i)
        different = different || first[i].x != second[i].x || first[i].y != second[i].y;
    EXPECT_TRUE(different);
    s = scan("clean_horizontal");
    s.defects = {JaggedBoundary{Bounds{{49, 59}, {51, 61}}, false, 0, 0.01, 2, 0}};
    const auto triangle = part("triangle");
    const auto p = cloud(triangle, s);
    const auto lastRow = std::count_if(p.begin(), p.end(), [](Point point) { return point.y == 60; });
    EXPECT_EQ(lastRow, 2);
    intervals(triangle, Direction::Horizontal, 60, {{49.991668055324115, 50.008331944675885}});
    PartGeometry circle{Circle{{0, 0}, 1}, {}};
    s.lineStep = 1; s.defects = {JaggedBoundary{Bounds{{-2, -2}, {2, 2}}, false, 0, 0.1, 2, 0}};
    const auto contact = cloud(circle, s);
    EXPECT_EQ(std::count_if(contact.begin(), contact.end(), [](Point point) { return point.y == -1; }), 1);
    EXPECT_EQ(std::count_if(contact.begin(), contact.end(), [](Point point) { return point.y == 1; }), 1);
}

TEST(SyntheticArtifacts, ParametersAndOutsidePlacementAreValidated)
{
    const auto g = part("rectangle_one_circle");
    for (Bounds b : {Bounds{{0, 0}, {1, 1}}, Bounds{{100, 0}, {110, 10}}, Bounds{{90, 0}, {110, 10}},
                     Bounds{{-1, -1}, {101, 61}}, Bounds{{49, 29}, {51, 31}}})
        EXPECT_THROW(validateOutsideRegion(g, b), Error);
    EXPECT_NO_THROW(validateOutsideRegion(g, {{101, 0}, {110, 10}}));
    auto root = QJsonDocument::fromJson(bytes(fixture("scans", "missing_points"))).object();
    for (const auto &bad : {QJsonObject{{"probability", -0.1}}, QJsonObject{{"probability", 1.1}},
                           QJsonObject{{"minRunLength", 0}}, QJsonObject{{"maxRunLength", 4}},
                           QJsonObject{{"minRunLength", 3}, {"maxRunLength", 2}},
                           QJsonObject{{"region", QJsonObject{{"min", QJsonArray{2, 2}}, {"max", QJsonArray{1, 1}}}}},
                           QJsonObject{{"preserveEndpoints", false}}}) {
        auto defect = root["defects"].toArray()[0].toObject();
        for (auto it = bad.begin(); it != bad.end(); ++it) defect[it.key()] = it.value();
        auto invalid = root; invalid["defects"] = QJsonArray{defect};
        EXPECT_THROW(parseScan(QJsonDocument(invalid).toJson()), Error);
    }
    auto s = scan("jagged_boundary");
    auto &j = std::get<JaggedBoundary>(s.defects[0]); j.inner = true; j.innerIndex = 1;
    EXPECT_THROW(validateScenario(g, s), Error); j.innerIndex = 0; j.randomAmplitude = -1;
    EXPECT_THROW(validateScenario(g, s), Error);
}

TEST(SyntheticArtifacts, CleanScenariosIgnoreSeedAndMixedUsesOrdinaryComposition)
{
    PartGeometry small{rectangle(0, 0, 5, 5), {}};
    for (const std::string name : {"clean_horizontal", "clean_vertical", "short_segments_horizontal", "clean_horizontal_vertical"}) {
        auto s = scan(name); const auto first = cloud(small, s); ++s.seed; equalClouds(first, cloud(small, s));
    }
    const auto g = part("complex_part");
    const auto s = scan("mixed_artifacts");
    std::vector<Point> composed;
    for (std::size_t i = 0; i < scanPasses(s).size(); ++i)
        generatePass(g, s, i, [&](Point p) { composed.push_back(p); });
    for (std::size_t i = 0; i < s.defects.size(); ++i)
        generateStandalone(s, i, [&](Point p) { composed.push_back(p); });
    equalClouds(cloud(g, s), composed);
}

TEST(SyntheticArtifacts, InlineRandomStreamsPreserveOtherDefectEndpoints)
{
    PartGeometry g{rectangle(0, 0, 10, 10), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 0.5;
    s.defects = {JaggedBoundary{Bounds{{-1, 0}, {1, 10}}, false, 0, 0.2, 2, 0.1}};
    const auto expected = cloud(g, s);
    const auto seed = randomStreamSeed(s, 0, 0);
    s.defects.insert(s.defects.begin(), MissingPoints{std::nullopt, 0.3, 1, 3});
    const auto actual = cloud(g, s);
    EXPECT_EQ(seed, randomStreamSeed(s, 0, 1));
    EXPECT_LT(actual.size(), expected.size());
    for (double y = 0; y <= 10; ++y) {
        const auto endpoint = std::find_if(expected.begin(), expected.end(), [y](Point p) { return p.y == y; });
        ASSERT_NE(endpoint, expected.end());
        EXPECT_TRUE(hasPoint(actual, endpoint->x, endpoint->y));
        EXPECT_TRUE(hasPoint(actual, 10, y));
    }
}

TEST(SyntheticStreaming, CompositeGenerationNeedsOnlyAnIncrementalSink)
{
    PartGeometry g{rectangle(0, 0, 200, 200), {}};
    auto s = scan("clean_horizontal"); s.pointStep = 0.5; s.lineStep = 0.5; s.maxLineLength = 10;
    s.passes = {{Direction::Horizontal, std::nullopt, 0, {}, {}},
                {Direction::Vertical, std::nullopt, 0, {}, {0.125, 0.0625}}};
    s.defects = {MissingPoints{std::nullopt, 0, 1, 3},
                 JaggedBoundary{Bounds{{-1, 300}, {1, 301}}, false, 0, 0.2, 2, 0.1},
                 OutsideGridCloud{Bounds{{210, 0}, {220, 10}}, 1, 1},
                 ExtraTableFragment{Bounds{{230, 0}, {240, 10}}}};
    std::int64_t count = 0;
    Bounds bounds{{1e100, 1e100}, {-1e100, -1e100}};
    generate(g, s, [&](Point p) {
        ++count;
        bounds.min.x = std::min(bounds.min.x, p.x); bounds.min.y = std::min(bounds.min.y, p.y);
        bounds.max.x = std::max(bounds.max.x, p.x); bounds.max.y = std::max(bounds.max.y, p.y);
    });
    EXPECT_EQ(count, 401 * 401 + 401 * 420 + 121 + 441);
    EXPECT_DOUBLE_EQ(bounds.min.x, 0); EXPECT_DOUBLE_EQ(bounds.min.y, 0);
    EXPECT_DOUBLE_EQ(bounds.max.x, 240); EXPECT_DOUBLE_EQ(bounds.max.y, 202.375);
}

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

TEST(SyntheticJson, VersionsNonfiniteValuesAndNestedUnknownFieldsAreRejected)
{
    auto geometry = QJsonDocument::fromJson(bytes(fixture("geometry", "circle"))).object();
    auto scenario = QJsonDocument::fromJson(bytes(fixture("scans", "clean_horizontal"))).object();
    for (const auto value : {QJsonValue(0), QJsonValue(2), QJsonValue(1.5), QJsonValue("1"), QJsonValue()}) {
        auto g = geometry; g["formatVersion"] = value;
        auto s = scenario; s["formatVersion"] = value;
        EXPECT_THROW(parseGeometry(QJsonDocument(g).toJson()), Error);
        EXPECT_THROW(parseScan(QJsonDocument(s).toJson()), Error);
    }
    for (const auto token : {"NaN", "Infinity", "-Infinity", "1e400"}) {
        auto input = bytes(fixture("scans", "clean_horizontal")); input.replace("0.07", token);
        EXPECT_THROW(parseScan(input), Error);
    }
    for (const auto bad : {QJsonObject{{"unknown", 1}}, QJsonObject{{"center", QJsonArray{0, 0, 0}}},
                           QJsonObject{{"radius", "50"}}}) {
        auto g = geometry; auto outer = g["outerContour"].toObject();
        for (auto it = bad.begin(); it != bad.end(); ++it) outer[it.key()] = it.value();
        g["outerContour"] = outer;
        EXPECT_THROW(parseGeometry(QJsonDocument(g).toJson()), Error);
    }
    for (const auto bad : {QJsonObject{{"type", "GaussianNoise"}},
                           QJsonObject{{"type", "JaggedBoundary"}, {"target", "invalid"}},
                           QJsonObject{{"type", "OutsideGridCloud"}, {"unknown", 1}}}) {
        auto s = scenario; s["defects"] = QJsonArray{bad};
        EXPECT_THROW(parseScan(QJsonDocument(s).toJson()), Error);
    }
    scenario["passes"] = QJsonArray{QJsonObject{{"continuationShift", QJsonObject{{"longitudinal", 0}}}}};
    EXPECT_THROW(parseScan(QJsonDocument(scenario).toJson()), Error);
}

TEST(SyntheticJson, AllArtifactFixturesParse)
{
    for (const std::string name : {"shifted_passes", "overlapping_passes", "double_scan", "outside_grid_cloud",
                                   "jagged_boundary", "extra_table_fragment", "mixed_artifacts", "missing_points"})
        EXPECT_NO_THROW(validateScenario(part("complex_part"), scan(name))) << name;
}

TEST(SyntheticJson, CleanPassDirectionsInheritanceAndValidation)
{
    const auto mixed = scan("clean_horizontal_vertical");
    EXPECT_EQ(scanDirections(mixed), (std::vector<Direction>{Direction::Horizontal, Direction::Vertical}));
    EXPECT_DOUBLE_EQ(mixed.pointStep, 0.07); EXPECT_DOUBLE_EQ(mixed.lineStep, 1);
    auto s = QJsonDocument::fromJson(bytes(fixture("scans", "clean_vertical"))).object();
    s["passes"] = QJsonArray{QJsonObject{}, QJsonObject{{"direction", "Horizontal"}}, QJsonObject{}};
    EXPECT_EQ(scanDirections(parseScan(QJsonDocument(s).toJson())),
              (std::vector<Direction>{Direction::Vertical, Direction::Horizontal, Direction::Vertical}));
    for (const auto &value : {QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{}),
                              QJsonValue(QJsonArray{1}), QJsonValue(QJsonArray{QJsonObject{{"direction", "Diagonal"}}}),
                              QJsonValue(QJsonArray{QJsonObject{{"direction", 1}}}),
                              QJsonValue(QJsonArray{QJsonObject{{"pointStep", 0.1}}}),
                              QJsonValue(QJsonArray{QJsonObject{{"unknown", true}}})}) {
        s["passes"] = value;
        EXPECT_THROW(parseScan(QJsonDocument(s).toJson()), Error);
    }
    for (const char *feature : {"region", "lineOffset", "longitudinalShift", "transverseShift", "continuationShift"}) {
        s["passes"] = QJsonArray{QJsonObject{{feature, "invalid"}}};
        try {
            parseScan(QJsonDocument(s).toJson());
            FAIL() << feature;
        } catch (const Error &e) {
            EXPECT_NE(std::string(e.what()).find(std::string("scan.passes[0].") + feature), std::string::npos);
        }
    }
}

TEST(SyntheticScan, CleanPassesConcatenateInOrderWithoutDeduplication)
{
    PartGeometry g{rectangle(0, 0, 2, 2), {}};
    auto s = scan("clean_horizontal_vertical");
    std::vector<Point> actual, expected;
    generate(g, s, [&](Point p) { actual.push_back(p); });
    s.passes.clear();
    s.direction = Direction::Horizontal;
    generate(g, s, [&](Point p) { expected.push_back(p); });
    s.direction = Direction::Vertical;
    generate(g, s, [&](Point p) { expected.push_back(p); });
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_DOUBLE_EQ(actual[i].x, expected[i].x); EXPECT_DOUBLE_EQ(actual[i].y, expected[i].y);
    }
    EXPECT_EQ(std::count_if(actual.begin(), actual.end(), [](Point p) { return p.x == 0 && p.y == 0; }), 2);
}

TEST(SyntheticGeometry, PerformancePlateBoundsAndHoleIntervals)
{
    const auto root = QStringLiteral(SYNTHETIC_FIXTURES) + "/performance/";
    const auto g = parseGeometry(bytes(root + "large_plate.geometry.json"));
    const auto s = parseScan(bytes(root + "clean_horizontal.scan.json"));
    const auto b = geometryBounds(g);
    EXPECT_DOUBLE_EQ(b.min.x, 0); EXPECT_DOUBLE_EQ(b.min.y, 0);
    EXPECT_DOUBLE_EQ(b.max.x, 3000); EXPECT_DOUBLE_EQ(b.max.y, 1000);
    intervals(g, Direction::Horizontal, 0, {{50, 2950}});
    intervals(g, Direction::Horizontal, 500, {{0, 650}, {850, 2150}, {2350, 3000}});
    EXPECT_EQ(scanDirections(s), (std::vector<Direction>{Direction::Horizontal}));
    EXPECT_DOUBLE_EQ(s.pointStep, 0.07); EXPECT_DOUBLE_EQ(s.lineStep, 1);
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
RunResult run(const QStringList &args, const QString &workingDirectory = {})
{
    QProcess process;
    if (!workingDirectory.isEmpty()) process.setWorkingDirectory(workingDirectory);
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
    const auto passes = m["passes"].toArray();
    const auto directions = scanDirections(s);
    ASSERT_EQ(passes.size(), directions.size());
    qint64 passCount = 0;
    for (qsizetype i = 0; i < passes.size(); ++i) {
        const auto pass = passes[i].toObject();
        EXPECT_EQ(pass["direction"].toString(), directions[i] == Direction::Horizontal ? "Horizontal" : "Vertical");
        EXPECT_GT(pass["pointCount"].toInteger(), 0);
        passCount += pass["pointCount"].toInteger();
    }
    for (const auto d : m["defects"].toArray()) passCount += d.toObject()["standalonePointCount"].toInteger();
    EXPECT_EQ(passCount, m["pointCount"].toInteger());
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
    if (geometry == "rectangle" && (scenario.starts_with("clean_") || scenario == "short_segments_horizontal"))
        EXPECT_EQ(count, scenario == "clean_horizontal" ? 87230 : scenario == "clean_vertical" ? 86759
                         : scenario == "clean_horizontal_vertical" ? 87230 + 86759 : 87413);
    if (scenario.starts_with("clean_") || scenario == "short_segments_horizontal") {
        const auto baseline = QJsonDocument::fromJson(bytes(QStringLiteral(SYNTHETIC_FIXTURES) + "/clean-output-hashes.json")).object()["outputs"].toArray();
        bool found = false;
        for (const auto value : baseline) {
            const auto entry = value.toObject();
            if (entry["geometry"].toString().toStdString() == geometry && entry["scan"].toString().toStdString() == scenario) {
                EXPECT_EQ(sha256(content), entry["sha256"].toString().toLatin1());
                EXPECT_EQ(count, entry["pointCount"].toInteger());
                found = true;
            }
        }
        EXPECT_TRUE(found);
    }
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
                                         testing::Values("clean_horizontal", "clean_vertical", "short_segments_horizontal",
                                                         "clean_horizontal_vertical")),
                        [](const auto &info) { return std::get<0>(info.param) + "_" + std::get<1>(info.param); });

class ArtifactDataset : public testing::TestWithParam<std::string> {};
TEST_P(ArtifactDataset, CliCoordinatesManifestAndByteDeterminism)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto name = GetParam();
    const auto output = directory.filePath("artifact.xyz");
    const auto result = run(args("complex_part", name, output));
    ASSERT_EQ(result.exitCode, 0) << result.error.toStdString();
    checkDataset(output, "complex_part", name);
    const auto content = bytes(output), manifest = bytes(output + ".manifest.json");
    const auto m = QJsonDocument::fromJson(manifest).object();
    const auto s = scan(name);
    const auto passes = scanPasses(s);
    for (std::size_t i = 0; i < passes.size(); ++i) {
        const auto p = m["passes"].toArray()[static_cast<qsizetype>(i)].toObject();
        EXPECT_DOUBLE_EQ(p["lineOffset"].toDouble(), passes[i].lineOffset);
        EXPECT_DOUBLE_EQ(p["longitudinalShift"].toDouble(), passes[i].shift.longitudinal);
        EXPECT_DOUBLE_EQ(p["transverseShift"].toDouble(), passes[i].shift.transverse);
        EXPECT_DOUBLE_EQ(p["continuationShift"].toObject()["longitudinal"].toDouble(), passes[i].continuation.longitudinal);
        if (passes[i].region) {
            const auto r = p["region"].toObject();
            EXPECT_DOUBLE_EQ(r["min"].toArray()[0].toDouble(), passes[i].region->min.x);
            EXPECT_DOUBLE_EQ(r["max"].toArray()[1].toDouble(), passes[i].region->max.y);
        } else EXPECT_TRUE(p["region"].isNull());
    }
    const auto input = QJsonDocument::fromJson(bytes(fixture("scans", name))).object();
    ASSERT_EQ(m["defects"].toArray().size(), s.defects.size());
    for (qsizetype i = 0; i < m["defects"].toArray().size(); ++i) {
        const auto d = m["defects"].toArray()[i].toObject();
        EXPECT_EQ(d["type"].toString(), input["defects"].toArray()[i].toObject()["type"].toString());
        if (d["type"] == "MissingPoints" || d["type"] == "JaggedBoundary") {
            ASSERT_EQ(d["randomStreams"].toArray().size(), passes.size());
            EXPECT_EQ(d["randomStreams"].toArray()[0].toObject()["seedHex"].toString().size(), 16);
        }
        if (d["type"] == "OutsideGridCloud") EXPECT_EQ(d["standalonePointCount"].toInteger(), 441);
        if (d["type"] == "ExtraTableFragment") EXPECT_EQ(d["standalonePointCount"].toInteger(), 26230);
        if (d["type"] == "MissingPoints") {
            EXPECT_DOUBLE_EQ(d["probability"].toDouble(), 0.01);
            EXPECT_EQ(d["minRunLength"].toInt(), 1); EXPECT_EQ(d["maxRunLength"].toInt(), 3);
        }
    }
    ASSERT_TRUE(QFile::remove(output)); ASSERT_TRUE(QFile::remove(output + ".manifest.json"));
    const auto repeated = run(args("complex_part", name, output));
    ASSERT_EQ(repeated.exitCode, 0) << repeated.error.toStdString();
    EXPECT_EQ(content, bytes(output)); EXPECT_EQ(manifest, bytes(output + ".manifest.json"));
}
INSTANTIATE_TEST_SUITE_P(ArtifactMatrix, ArtifactDataset,
                        testing::Values("shifted_passes", "overlapping_passes", "double_scan", "missing_points",
                                        "outside_grid_cloud", "jagged_boundary", "extra_table_fragment", "mixed_artifacts"),
                        [](const auto &info) { return info.param; });

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

TEST(SyntheticIntegration, MultiPassEqualsIndependentFilesAndIsByteDeterministic)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto h = directory.filePath("horizontal.xyz"), v = directory.filePath("vertical.xyz");
    const auto combined = directory.filePath("combined.xyz");
    for (const auto &[scenario, output] : std::vector<std::pair<std::string, QString>>{
             {"clean_horizontal", h}, {"clean_vertical", v}, {"clean_horizontal_vertical", combined}}) {
        const auto result = run(args("complex_part", scenario, output));
        ASSERT_EQ(result.exitCode, 0) << result.error.toStdString();
    }
    const auto content = bytes(combined), manifest = bytes(combined + ".manifest.json");
    EXPECT_EQ(content, bytes(h) + bytes(v));
    EXPECT_EQ(content.split('\n').count(QByteArray("10.000000 0.000000 0")), 2);
    const auto m = QJsonDocument::fromJson(manifest).object();
    const auto passes = m["passes"].toArray();
    ASSERT_EQ(passes.size(), 2);
    EXPECT_EQ(passes[0].toObject()["pointCount"].toInteger(), 168374);
    EXPECT_EQ(passes[1].toObject()["pointCount"].toInteger(), 167482);
    EXPECT_EQ(m["pointCount"].toInteger(), 168374 + 167482);
    ASSERT_TRUE(QFile::remove(combined)); ASSERT_TRUE(QFile::remove(combined + ".manifest.json"));
    ASSERT_EQ(run(args("complex_part", "clean_horizontal_vertical", combined)).exitCode, 0);
    EXPECT_EQ(content, bytes(combined)); EXPECT_EQ(manifest, bytes(combined + ".manifest.json"));
}

TEST(SyntheticIntegration, LegacyCleanOutputHashesRemainUnchanged)
{
    // Recorded from generator 0.2.0 on the supported Release toolchain.
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    for (const auto &[scenario, hash] : std::vector<std::pair<std::string, QByteArray>>{
             {"clean_horizontal", "538684d0cb1130be7f9441db3fcf0a0e1289fd4704633abed5a897ef188e41f8"},
             {"clean_vertical", "032304ace0a726082e6dec370d860a1cd5362110300d9f00b63f7c39aa72b366"},
             {"short_segments_horizontal", "2142bec90dc112e431c25f0e01fd5ede941ec0904fe66b93144ed30debe4a3ef"}}) {
        const auto output = directory.filePath(QString::fromStdString(scenario) + ".xyz");
        ASSERT_EQ(run(args("rectangle", scenario, output)).exitCode, 0);
        EXPECT_EQ(sha256(bytes(output)), hash);
    }
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
    result = run(args("rectangle", "double_scan", directory.filePath("advanced.bin")));
    EXPECT_NE(result.exitCode, 0); EXPECT_TRUE(result.out.isEmpty()); EXPECT_TRUE(result.error.contains("extension"));
    EXPECT_FALSE(QFile::exists(directory.filePath("advanced.bin")));
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

TEST(SyntheticIntegration, EmptyPassAndArtifactFailurePublishConsistentResults)
{
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    auto root = QJsonDocument::fromJson(bytes(fixture("scans", "clean_horizontal"))).object();
    root["passes"] = QJsonArray{QJsonObject{{"region", QJsonObject{{"min", QJsonArray{200, 200}}, {"max", QJsonArray{210, 210}}}}}};
    const auto config = directory.filePath("scan.json"), output = directory.filePath("empty.xyz");
    const auto save = [&] {
        QFile file(config); EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        const auto data = QJsonDocument(root).toJson(); EXPECT_EQ(file.write(data), data.size());
    };
    save();
    auto a = args("rectangle", "clean_horizontal", output); a[3] = config;
    ASSERT_EQ(run(a).exitCode, 0);
    EXPECT_TRUE(bytes(output).isEmpty());
    const auto m = QJsonDocument::fromJson(bytes(output + ".manifest.json")).object();
    EXPECT_EQ(m["pointCount"].toInteger(), 0); EXPECT_TRUE(m["boundingBox"].isNull());
    EXPECT_EQ(m["outputSha256"].toString().toLatin1(), sha256({}));
    root["maxLineLength"] = 20;
    root["passes"] = QJsonArray{QJsonObject{{"longitudinalShift", 1e308},
                                          {"continuationShift", QJsonObject{{"longitudinal", 1e308}, {"transverse", 0}}}}};
    save(); a[5] = directory.filePath("overflow.xyz");
    const auto failure = run(a);
    EXPECT_NE(failure.exitCode, 0); EXPECT_TRUE(failure.out.isEmpty());
    EXPECT_FALSE(QFile::exists(a[5])); EXPECT_FALSE(QFile::exists(a[5] + ".manifest.json"));
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files).size(), 3);
}

TEST(SyntheticIntegration, ExitCodesInputFailuresAndInvalidOutputParent)
{
    for (const QStringList a : {QStringList{}, QStringList{"--help", "extra"},
                                QStringList{"--geometry"}, QStringList{"--unknown", "x"},
                                QStringList{"--geometry", "x", "--geometry", "y"}}) {
        const auto r = run(a); EXPECT_EQ(r.exitCode, 2); EXPECT_TRUE(r.out.isEmpty()); EXPECT_FALSE(r.error.isEmpty());
    }
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto output = directory.filePath("part.xyz");
    auto a = args("rectangle", "clean_horizontal", output);
    const auto success = run(a, directory.path());
    ASSERT_EQ(success.exitCode, 0); EXPECT_FALSE(success.out.isEmpty()); EXPECT_TRUE(success.error.isEmpty());
    checkDataset(output, "rectangle", "clean_horizontal");
    const auto saved = bytes(output), metadata = bytes(output + ".manifest.json");
    const auto inputPath = directory.filePath("invalid.json");
    QFile input(inputPath); ASSERT_TRUE(input.open(QIODevice::WriteOnly)); ASSERT_EQ(input.write("{"), 1); input.close();
    for (int field : {1, 3}) {
        a = args("rectangle", "clean_horizontal", directory.filePath("failure.xyz")); a[field] = inputPath;
        auto r = run(a); EXPECT_EQ(r.exitCode, 3); EXPECT_TRUE(r.out.isEmpty()); EXPECT_TRUE(r.error.contains("invalid.json"));
        a[field] = directory.filePath("absent.json");
        r = run(a); EXPECT_EQ(r.exitCode, 4); EXPECT_TRUE(r.out.isEmpty()); EXPECT_TRUE(r.error.contains("absent.json"));
    }
    a = args("rectangle", "clean_horizontal", inputPath + "/part.xyz");
    auto r = run(a); EXPECT_EQ(r.exitCode, 4); EXPECT_TRUE(r.out.isEmpty()); EXPECT_TRUE(r.error.contains("temporary"));
    EXPECT_EQ(bytes(inputPath), "{");
    a[5] = directory.filePath("missing/part.xyz"); r = run(a);
    EXPECT_EQ(r.exitCode, 4); EXPECT_TRUE(r.out.isEmpty());
    a[5] = output; EXPECT_EQ(run(a).exitCode, 4);
    EXPECT_EQ(bytes(output), saved); EXPECT_EQ(bytes(output + ".manifest.json"), metadata);
    ASSERT_TRUE(QFile::remove(output)); EXPECT_EQ(run(a).exitCode, 4);
    EXPECT_EQ(bytes(output + ".manifest.json"), metadata);
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files).size(), 2);
}

TEST(SyntheticIntegration, CommaLocaleDoesNotChangeSerialization)
{
    struct CommaPunctuation : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    struct RestoreLocale {
        std::locale standard = std::locale();
        QLocale qt = QLocale();
        ~RestoreLocale() { std::locale::global(standard); QLocale::setDefault(qt); }
    } restore;
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    const auto output = directory.filePath("part.xyz");
    const auto g = fixture("geometry", "rectangle"), s = fixture("scans", "clean_horizontal");
    writeDataset(g, s, output);
    const auto expected = bytes(output), manifest = bytes(output + ".manifest.json");
    ASSERT_TRUE(QFile::remove(output)); ASSERT_TRUE(QFile::remove(output + ".manifest.json"));
    std::locale::global(std::locale(std::locale::classic(), new CommaPunctuation));
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    EXPECT_EQ(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point(), ',');
    EXPECT_EQ(QLocale().decimalPoint(), ",");
    writeDataset(g, s, output);
    EXPECT_EQ(bytes(output), expected); EXPECT_EQ(bytes(output + ".manifest.json"), manifest);
    checkDataset(output, "rectangle", "clean_horizontal");
}

} // namespace
