#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace cadcontour {

// Persistent logical identity, independent of runtime stamps and dataset identities.
struct ProjectId {
    std::array<std::byte, 16> bytes{}; // UUID in RFC 4122 byte order
    bool isNull() const noexcept
    {
        return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{0}; });
    }
    bool operator==(const ProjectId &) const = default;
};

} // namespace cadcontour
