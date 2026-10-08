#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace synthetic {

inline constexpr char generatorVersion[] = "0.2.0";

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
struct ScanScenario
{
    Direction direction;
    double pointStep;
    double lineStep;
    double maxLineLength;
    std::uint32_t seed;
};
// A zero-width interval represents one isolated geometric contact.
struct Interval { double min; double max; };
using PointSink = std::function<void(Point)>;

PartGeometry parseGeometry(const QByteArray &json);
ScanScenario parseScan(const QByteArray &json);
void validateGeometry(const PartGeometry &geometry);
Bounds geometryBounds(const PartGeometry &geometry);
std::vector<Interval> materialIntervals(const PartGeometry &geometry,
                                      Direction direction, double coordinate);
void sampleInterval(Interval interval, double maxLineLength, double pointStep,
                    const std::function<void(double)> &sink);
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
