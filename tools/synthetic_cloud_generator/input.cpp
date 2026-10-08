#include "generator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cmath>
#include <initializer_list>
#include <numbers>

namespace synthetic {
namespace {

[[noreturn]] void invalid(const QString &path, const char *message)
{
    throw Error((path + ": " + message).toStdString());
}

QJsonObject object(const QJsonValue &value, const QString &path,
                   std::initializer_list<const char *> allowed)
{
    if (!value.isObject())
        invalid(path, "expected object");
    const auto result = value.toObject();
    for (auto it = result.begin(); it != result.end(); ++it) {
        bool known = false;
        for (const auto name : allowed)
            known |= it.key() == QLatin1StringView(name);
        if (!known)
            invalid(path + "." + it.key(), "unknown field");
    }
    return result;
}

QJsonValue document(const QByteArray &json)
{
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError)
        throw Error(("JSON at byte " + QString::number(error.offset) + ": "
                     + error.errorString()).toStdString());
    if (!doc.isObject())
        invalid("root", "expected object");
    return doc.object();
}

double number(const QJsonValue &value, const QString &path, bool positive = false)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        invalid(path, "expected finite number");
    const double result = value.toDouble();
    if (positive && result <= 0)
        invalid(path, "must be positive");
    return result;
}

double integer(const QJsonValue &value, const QString &path, double max)
{
    const double result = number(value, path);
    if (result < 0 || result > max || std::trunc(result) != result)
        invalid(path, "expected integer in supported range");
    return result;
}

QString string(const QJsonValue &value, const QString &path)
{
    if (!value.isString())
        invalid(path, "expected string");
    return value.toString();
}

Point point(const QJsonValue &value, const QString &path)
{
    if (!value.isArray() || value.toArray().size() != 2)
        invalid(path, "expected two-number array");
    const auto a = value.toArray();
    return {number(a[0], path + "[0]"), number(a[1], path + "[1]")};
}

Contour contour(const QJsonValue &value, const QString &path)
{
    if (!value.isObject())
        invalid(path, "expected object");
    const auto type = string(value.toObject()["type"], path + ".type");
    if (type == "CIRCLE") {
        const auto o = object(value, path, {"type", "center", "radius"});
        return Circle{point(o["center"], path + ".center"),
                      number(o["radius"], path + ".radius", true)};
    }
    if (type != "CHAIN")
        invalid(path + ".type", "expected CHAIN or CIRCLE");
    const auto o = object(value, path, {"type", "segments"});
    if (!o["segments"].isArray() || o["segments"].toArray().isEmpty())
        invalid(path + ".segments", "expected nonempty array");
    std::vector<Segment> segments;
    const auto array = o["segments"].toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QString p = path + ".segments[" + QString::number(i) + "]";
        if (!array[i].isObject())
            invalid(p, "expected object");
        const auto t = string(array[i].toObject()["type"], p + ".type");
        if (t == "LINE") {
            const auto s = object(array[i], p, {"type", "start", "end"});
            segments.emplace_back(Line{point(s["start"], p + ".start"),
                                       point(s["end"], p + ".end")});
        } else if (t == "ARC") {
            const auto s = object(array[i], p,
                                  {"type", "center", "radius", "startAngle", "sweepAngle"});
            Arc a{point(s["center"], p + ".center"), number(s["radius"], p + ".radius", true),
                  number(s["startAngle"], p + ".startAngle"),
                  number(s["sweepAngle"], p + ".sweepAngle")};
            if (a.sweepAngle == 0 || std::abs(a.sweepAngle) >= 2 * std::numbers::pi)
                invalid(p + ".sweepAngle", "must satisfy 0 < abs(sweepAngle) < 2*pi");
            segments.emplace_back(a);
        } else {
            invalid(p + ".type", "unsupported primitive (expected LINE or ARC)");
        }
    }
    return segments;
}

} // namespace

PartGeometry parseGeometry(const QByteArray &json)
{
    const auto o = object(document(json), "geometry",
                          {"formatVersion", "units", "outerContour", "innerContours"});
    if (integer(o["formatVersion"], "formatVersion", 1) != 1)
        invalid("formatVersion", "expected 1");
    if (string(o["units"], "units") != "mm")
        invalid("units", "expected mm");
    if (!o["innerContours"].isArray())
        invalid("innerContours", "expected array");
    PartGeometry result{contour(o["outerContour"], "outerContour"), {}};
    const auto inner = o["innerContours"].toArray();
    for (qsizetype i = 0; i < inner.size(); ++i)
        result.inner.push_back(contour(inner[i], "innerContours[" + QString::number(i) + "]"));
    validateGeometry(result);
    return result;
}

ScanScenario parseScan(const QByteArray &json)
{
    const auto o = object(document(json), "scan",
                          {"formatVersion", "direction", "pointStep", "lineStep",
                           "maxLineLength", "seed", "passes", "defects"});
    if (integer(o["formatVersion"], "scan.formatVersion", 1) != 1)
        invalid("scan.formatVersion", "expected 1");
    const auto direction = string(o["direction"], "scan.direction");
    if (direction != "Horizontal" && direction != "Vertical")
        invalid("scan.direction", "expected Horizontal or Vertical");
    ScanScenario result{direction == "Horizontal" ? Direction::Horizontal : Direction::Vertical,
                        number(o["pointStep"], "scan.pointStep", true),
                        number(o["lineStep"], "scan.lineStep", true),
                        number(o["maxLineLength"], "scan.maxLineLength", true),
                        static_cast<std::uint32_t>(integer(o["seed"], "scan.seed", 4294967295.0))};
    if (o.contains("passes"))
        invalid("scan.passes", "unsupported in current generator implementation");
    if (o.contains("defects")) {
        if (!o["defects"].isArray())
            invalid("scan.defects", "expected array");
        if (!o["defects"].toArray().isEmpty())
            invalid("scan.defects", "unsupported in current generator implementation");
    }
    return result;
}

} // namespace synthetic
