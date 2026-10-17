#include "asc_xyz_parser.h"

#include <QFileDevice>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

namespace cadcontour::io {
namespace {
using namespace cache;
bool space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
Error error(ErrorCode code, const char *detail, Phase phase = Phase::Parse)
{
    return makeError(code, phase, phase == Phase::SourceRead ? Operation::Read : Operation::Decode,
                     QString::fromLatin1(detail));
}
QString excerpt(std::string_view token)
{
    QString result;
    for (const unsigned char c : token.substr(0, 64)) {
        if (c >= 32 && c <= 126) result += QChar(c);
        else result += QStringLiteral("\\x%1").arg(c, 2, 16, QLatin1Char('0'));
    }
    return result;
}
Error located(ErrorCode code, const char *detail, std::uint64_t line, std::size_t column,
              std::optional<Coordinate> coordinate = {}, std::string_view token = {})
{
    auto e = error(code, detail);
    e.source = SourceLocation{line, static_cast<std::uint32_t>(column + 1), coordinate, excerpt(token)};
    return e;
}
Result<double> number(std::string_view token, std::uint64_t line, std::size_t column, Coordinate coordinate)
{
    std::size_t i = 0;
    if (i < token.size() && (token[i] == '+' || token[i] == '-')) ++i;
    bool any = false, nonzero = false;
    auto digits = [&] {
        while (i < token.size() && digit(token[i])) {
            any = true; nonzero |= token[i] != '0'; ++i;
        }
    };
    digits();
    if (i < token.size() && token[i] == '.') { ++i; digits(); }
    bool valid = any;
    if (i < token.size() && (token[i] == 'e' || token[i] == 'E')) {
        ++i;
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) ++i;
        const auto start = i;
        while (i < token.size() && digit(token[i])) ++i;
        valid &= i != start;
    }
    if (!valid || i != token.size())
        return located(ErrorCode::ParseError, "Invalid decimal coordinate", line, column, coordinate, token);
    auto text = token;
    if (text.front() == '+') text.remove_prefix(1);
    double value = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size())
        return located(converted.ec == std::errc::result_out_of_range ? ErrorCode::NumericRange : ErrorCode::ParseError,
                       "Decimal conversion failed", line, column, coordinate, token);
    if (!std::isfinite(value) || (nonzero && value == 0.0))
        return located(ErrorCode::NumericRange, "Coordinate outside binary64 range", line, column, coordinate, token);
    return value;
}
struct Buffers {
    std::array<char, ascReadBytes> read;
    std::array<char, ascLineBytes> line;
    std::array<Measurement, ascBatchSize> batch;
};
static_assert(sizeof(Buffers) <= ascParserReserve);

class Parser {
public:
    Parser(QIODevice &source, const MeasurementSink &sink, const ZHandler &zHandler,
           CancelCheck cancel, void *context)
        : source_(source), sink_(sink), zHandler_(zHandler), cancel_(cancel), context_(context) {}
    Result<SourceSummary> run();
private:
    Status checkpoint()
    {
        return cancel_ && cancel_(context_) ? Status{error(ErrorCode::Cancelled, "ASC/XYZ import cancelled")} : success();
    }
    Status flushBatch();
    Status line();
    Status byte(char c);
    QIODevice &source_;
    const MeasurementSink &sink_;
    const ZHandler &zHandler_;
    CancelCheck cancel_;
    void *context_;
    std::unique_ptr<Buffers> buffers_;
    SourceSummary summary_;
    std::size_t length_ = 0, used_ = 0;
    bool cr_ = false;
};
Status Parser::flushBatch()
{
    if (auto s = checkpoint(); !s) return s;
    if (used_) {
        Status s = success();
        try { s = sink_({buffers_->batch.data(), used_}); }
        catch (const std::bad_alloc &) { throw; }
        catch (...) { return error(ErrorCode::ConsumerFailed, "ASC/XYZ sink threw an exception"); }
        if (!s) return s;
        used_ = 0;
    }
    return checkpoint();
}
Status Parser::line()
{
    if (summary_.physicalLines == std::numeric_limits<std::uint64_t>::max())
        return error(ErrorCode::ResourceExhausted, "Physical line count overflow");
    const auto lineNumber = ++summary_.physicalLines;
    const std::string_view content(buffers_->line.data(), length_);
    std::array<std::string_view, 3> tokens;
    std::array<std::size_t, 3> columns{};
    std::size_t i = 0, count = 0;
    while (i < content.size()) {
        if (space(content[i])) { ++i; continue; }
        const auto start = i;
        while (i < content.size() && !space(content[i])) ++i;
        if (count == 3)
            return located(ErrorCode::ParseError, "Expected exactly three coordinates", lineNumber, start, {}, content.substr(start, i - start));
        columns[count] = start;
        tokens[count++] = content.substr(start, i - start);
    }
    if (!count) return success();
    if (count != 3)
        return located(ErrorCode::ParseError, "Missing coordinate", lineNumber, content.size(), static_cast<Coordinate>(count));
    Measurement m;
    const std::array<double *, 3> values{&m.x, &m.y, &m.z};
    for (std::size_t n = 0; n < 3; ++n) {
        auto result = number(tokens[n], lineNumber, columns[n], static_cast<Coordinate>(n));
        if (!result) return result.error();
        *values[n] = result.value();
    }
    if (m.z != 0.0 && !summary_.firstNonZeroZ) {
        summary_.firstNonZeroZ = NonZeroZ{lineNumber, m.z};
        if (!zHandler_) return error(ErrorCode::InvalidInput, "Nonzero Z requires a decision handler");
        if (auto s = checkpoint(); !s) return s;
        ZDecision decision;
        try { decision = zHandler_(*summary_.firstNonZeroZ); }
        catch (const std::bad_alloc &) { throw; }
        catch (...) { return error(ErrorCode::ConsumerFailed, "ASC/XYZ Z handler threw an exception"); }
        if (decision == ZDecision::Cancel) return error(ErrorCode::Cancelled, "Nonzero Z import declined");
        if (decision != ZDecision::Continue) return error(ErrorCode::InvalidState, "Invalid Z decision");
        if (auto s = checkpoint(); !s) return s;
    }
    if (summary_.measurements == std::numeric_limits<std::uint64_t>::max())
        return error(ErrorCode::ResourceExhausted, "Measurement count overflow");
    ++summary_.measurements;
    buffers_->batch[used_++] = m;
    return used_ == ascBatchSize ? flushBatch() : checkpoint();
}
Status Parser::byte(char c)
{
    if (cr_ && c != '\n')
        return located(ErrorCode::ParseError, "CR must be followed by LF", summary_.physicalLines + 1, length_);
    if (c == '\n') {
        cr_ = false;
        auto s = line(); length_ = 0;
        return s;
    }
    if (c == '\r') { cr_ = true; return success(); }
    const auto u = static_cast<unsigned char>(c);
    if ((!space(c) && (u < 33 || u > 126)))
        return located(ErrorCode::ParseError, "Unsupported source byte", summary_.physicalLines + 1, length_, {}, {&c, 1});
    if (length_ == ascLineBytes)
        return located(ErrorCode::ParseError, "Physical line exceeds 64 KiB", summary_.physicalLines + 1, length_);
    buffers_->line[length_++] = c;
    return success();
}
Result<SourceSummary> Parser::run()
{
    if (!source_.isOpen() || !source_.isReadable() || source_.isTextModeEnabled() || !sink_
        || (!source_.isSequential() && source_.pos() != 0))
        return error(ErrorCode::InvalidInput, "Expected binary source at beginning and a sink");
    if (auto s = checkpoint(); !s) return s.error();
    buffers_ = std::make_unique<Buffers>();
    unsigned bom = 0;
    bool beginning = true;
    constexpr std::array<unsigned char, 3> utf8{0xef, 0xbb, 0xbf};
    for (;;) {
        if (auto s = checkpoint(); !s) return s.error();
        const auto n = source_.read(buffers_->read.data(), ascReadBytes);
        if (auto s = checkpoint(); !s) return s.error();
        if (n < 0 || n > static_cast<qint64>(ascReadBytes) || (n == 0 && !source_.atEnd())) {
            const auto file = qobject_cast<QFileDevice *>(&source_);
            return makeError(ErrorCode::ReadFailed, Phase::SourceRead, Operation::Read,
                             source_.errorString(), summary_.bytesRead, file ? file->error() : 0);
        }
        if (n == 0) break;
        if (static_cast<std::uint64_t>(n) > std::numeric_limits<std::uint64_t>::max() - summary_.bytesRead)
            return error(ErrorCode::ResourceExhausted, "Source byte count overflow");
        summary_.bytesRead += static_cast<std::uint64_t>(n);
        for (qint64 i = 0; i < n; ++i) {
            if (i % 1024 == 0) { if (auto s = checkpoint(); !s) return s.error(); }
            const char c = buffers_->read[static_cast<std::size_t>(i)];
            if (beginning && (bom || static_cast<unsigned char>(c) == utf8[0])) {
                if (static_cast<unsigned char>(c) != utf8[bom])
                    return located(ErrorCode::ParseError, "Invalid initial UTF-8 BOM", 1, 0);
                if (++bom == utf8.size()) beginning = false;
                continue;
            }
            beginning = false;
            if (auto s = byte(c); !s) return s.error();
        }
    }
    if (beginning && bom) return located(ErrorCode::ParseError, "Incomplete UTF-8 BOM", 1, 0);
    if (cr_) return located(ErrorCode::ParseError, "CR must be followed by LF", summary_.physicalLines + 1, length_);
    if (length_) { if (auto s = line(); !s) return s.error(); }
    if (!summary_.measurements) return error(ErrorCode::InsufficientData, "Source has no measurements");
    if (auto s = flushBatch(); !s) return s.error();
    return summary_;
}
} // namespace

cache::Result<SourceSummary> parseAscXyz(QIODevice &source, const MeasurementSink &sink,
    const ZHandler &zHandler, CancelCheck cancel, void *context)
{
    try { return Parser(source, sink, zHandler, cancel, context).run(); }
    catch (const std::bad_alloc &) { return error(cache::ErrorCode::ResourceExhausted, "ASC/XYZ allocation failed"); }
}
} // namespace cadcontour::io
