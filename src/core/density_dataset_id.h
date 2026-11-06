#pragma once

#include <array>
#include <algorithm>
#include <cstddef>

namespace cadcontour::density {

// Logical D identity, shared by the domain descriptor and storage codec.
struct DatasetId {
    std::array<std::byte, 16> bytes{};
    static DatasetId create();
    bool isNull() const noexcept
    {
        return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{}; });
    }
    bool operator==(const DatasetId &) const = default;
};

} // namespace cadcontour::density
