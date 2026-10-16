#include "binary_codec.h"

#include <bit>
#include <climits>
#include <limits>

namespace cadcontour::cache {

static_assert(CHAR_BIT == 8 && sizeof(double) == 8);
static_assert(std::numeric_limits<double>::is_iec559
              && std::numeric_limits<double>::digits == 53
              && std::numeric_limits<double>::max_exponent == 1024);
static_assert(sizeof(std::uint64_t) == 8 && sizeof(std::uint32_t) == 4);

namespace {
template<class T>
bool put(MutableBytes bytes, std::size_t offset, T value) noexcept
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        return false;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
    return true;
}

template<class T>
std::optional<T> get(Bytes bytes, std::size_t offset) noexcept
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        return std::nullopt;
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(std::to_integer<unsigned>(bytes[offset + i])) << (i * 8);
    return value;
}
} // namespace

namespace binary {
bool putU32(MutableBytes b, std::size_t o, std::uint32_t v) noexcept { return put(b, o, v); }
bool putU64(MutableBytes b, std::size_t o, std::uint64_t v) noexcept { return put(b, o, v); }
bool putDouble(MutableBytes b, std::size_t o, double v) noexcept
{
    return putU64(b, o, std::bit_cast<std::uint64_t>(v));
}
std::optional<std::uint32_t> getU32(Bytes b, std::size_t o) noexcept { return get<std::uint32_t>(b, o); }
std::optional<std::uint64_t> getU64(Bytes b, std::size_t o) noexcept { return get<std::uint64_t>(b, o); }
std::optional<double> getDouble(Bytes b, std::size_t o) noexcept
{
    const auto bits = getU64(b, o);
    return bits ? std::optional{std::bit_cast<double>(*bits)} : std::nullopt;
}
} // namespace binary

namespace checked {
std::optional<std::uint64_t> add(std::uint64_t a, std::uint64_t b) noexcept
{
    if (b > UINT64_MAX - a) return std::nullopt;
    return a + b;
}
std::optional<std::uint64_t> multiply(std::uint64_t a, std::uint64_t b) noexcept
{
    if (a != 0 && b > UINT64_MAX / a) return std::nullopt;
    return a * b;
}
std::optional<std::uint64_t> pageCount(std::uint64_t count, std::uint64_t capacity) noexcept
{
    if (capacity == 0) return std::nullopt;
    return count / capacity + (count % capacity != 0 ? 1 : 0);
}
bool range(std::uint64_t offset, std::uint64_t length, std::uint64_t fileSize) noexcept
{
    return offset <= fileSize && length <= fileSize - offset;
}
std::optional<qint64> fileOffset(std::uint64_t value) noexcept
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()))
        return std::nullopt;
    return static_cast<qint64>(value);
}
std::optional<std::size_t> memorySize(std::uint64_t value) noexcept
{
    if (value > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    return static_cast<std::size_t>(value);
}
} // namespace checked
} // namespace cadcontour::cache
