#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace synthetic {

inline constexpr char generatorVersion[] = "0.4.0";

class Error : public std::runtime_error
{
public:
    explicit Error(const std::string &message, int exitCode = 3)
        : std::runtime_error(message), exitCode(exitCode) {}
    int exitCode;
};

struct Point { double x; double y; };
struct Line { Point start; Point end; };
struct Arc { Point center; double radius; double startAngle; double sweepAngle; };
struct Circle { Point center; double radius; };
using Segment = std::variant<Line, Arc>;
using Contour = std::variant<std::vector<Segment>, Circle>;
struct PartGeometry { Contour outer; std::vector<Contour> inner; };
struct Bounds { Point min; Point max; };
enum class Direction { Horizontal, Vertical };
struct Shift { double longitudinal = 0; double transverse = 0; };
struct ScanPass
{
    Direction direction;
    std::optional<Bounds> region;
    double lineOffset = 0;
    Shift shift;
    Shift continuation;
};
struct OutsideGridCloud { Bounds region; double pointStepX; double pointStepY; };
struct JaggedBoundary
{
    Bounds region;
    bool inner;
    std::size_t innerIndex;
    double amplitude;
    double toothStep;
    double randomAmplitude = 0;
};
struct ExtraTableFragment { Bounds region; };
struct MissingPoints
{
    std::optional<Bounds> region;
    double probability;
    std::uint32_t minRunLength = 1;
    std::uint32_t maxRunLength = 3;
};
using Defect = std::variant<OutsideGridCloud, JaggedBoundary, ExtraTableFragment, MissingPoints>;
struct ScanScenario
{
    Direction direction;
    double pointStep;
    double lineStep;
    double maxLineLength;
    std::uint32_t seed;
    std::vector<ScanPass> passes;
    std::vector<Defect> defects;
};
// A zero-width interval represents one isolated geometric contact.
struct Interval { double min; double max; };
using PointSink = std::function<void(Point)>;

PartGeometry parseGeometry(const QByteArray &json);
ScanScenario parseScan(const QByteArray &json);
void validateGeometry(const PartGeometry &geometry);
void validateScenario(const PartGeometry &geometry, const ScanScenario &scan);
void validateOutsideRegion(const PartGeometry &geometry, Bounds region);
Bounds geometryBounds(const PartGeometry &geometry);
std::vector<Interval> contourBoundaryIntervals(const Contour &contour, Direction direction, double coordinate);
std::vector<Interval> materialIntervals(const PartGeometry &geometry,
                                      Direction direction, double coordinate);
void sampleInterval(Interval interval, double maxLineLength, double pointStep,
                    const std::function<void(double)> &sink);
std::vector<Direction> scanDirections(const ScanScenario &scan);
std::vector<ScanPass> scanPasses(const ScanScenario &scan);
bool cleanPass(const ScanScenario &scan, const ScanPass &pass);
void generateCleanPass(const PartGeometry &geometry, const ScanScenario &scan,
                       Direction direction, const PointSink &sink);
void generatePass(const PartGeometry &geometry, const ScanScenario &scan,
                  std::size_t passIndex, const PointSink &sink);
void generateStandalone(const ScanScenario &scan, std::size_t defectIndex, const PointSink &sink);
std::uint64_t randomStreamSeed(const ScanScenario &scan, std::size_t passIndex, std::size_t defectIndex);
void validateOutputSegment(Point first, Point last);
void generate(const PartGeometry &geometry, const ScanScenario &scan, const PointSink &sink);

struct DatasetStats
{
    std::int64_t pointCount = 0;
    Bounds bounds{};
    QByteArray sha256;
};
DatasetStats writeDataset(const QString &geometryPath, const QString &scanPath,
                          const QString &outputPath);

} // namespace synthetic
