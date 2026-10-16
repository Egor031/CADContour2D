#include "cache_types.h"

#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cadcontour::cache {

CacheId CacheId::create()
{
    CacheId id;
    const auto bytes = QUuid::createUuid().toRfc4122();
    std::memcpy(id.bytes.data(), bytes.constData(), id.bytes.size());
    return id;
}

bool CacheId::isNull() const noexcept
{
    return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{0}; });
}

bool validPoint(ModelPoint point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool validBounds(BoundingBox b) noexcept
{
    return validPoint({b.minX, b.minY}) && validPoint({b.maxX, b.maxY})
        && b.minX <= b.maxX && b.minY <= b.maxY;
}

Error makeError(ErrorCode code, Phase phase, Operation operation, QString detail,
                std::optional<std::uint64_t> offset, int deviceCode)
{
    return {code, phase, operation, offset, deviceCode, detail.left(256)};
}

} // namespace cadcontour::cache
