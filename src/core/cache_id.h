#pragma once

#include <array>
#include <cstddef>

namespace cadcontour::cache {

// Value identity shared by domain metadata and the binary cache codec. No storage owner.
struct CacheId {
    std::array<std::byte, 16> bytes{}; // UUID in RFC 4122 byte order, never native QUuid memory
    static CacheId create();
    bool isNull() const noexcept;
    bool operator==(const CacheId &) const = default;
};

} // namespace cadcontour::cache
