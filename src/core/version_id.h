#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace cadcontour {

// Persistent version UUID; neither a project/dataset identity nor a runtime counter.
struct VersionId {
    std::array<std::byte, 16> bytes{};
    bool isNull() const noexcept
    {
        return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{}; });
    }
    bool operator==(const VersionId &) const = default;
};

} // namespace cadcontour
