#pragma once

#include "cache_types.h"

#include <QtTypes>

#include <span>

namespace cadcontour::cache {

using Bytes = std::span<const std::byte>;
using MutableBytes = std::span<std::byte>;

namespace binary {

bool putU32(MutableBytes bytes, std::size_t offset, std::uint32_t value) noexcept;
bool putU64(MutableBytes bytes, std::size_t offset, std::uint64_t value) noexcept;
bool putDouble(MutableBytes bytes, std::size_t offset, double value) noexcept;
std::optional<std::uint32_t> getU32(Bytes bytes, std::size_t offset) noexcept;
std::optional<std::uint64_t> getU64(Bytes bytes, std::size_t offset) noexcept;
// Raw bit decoding accepts all representations. Dataset validation is separate.
std::optional<double> getDouble(Bytes bytes, std::size_t offset) noexcept;

} // namespace binary

namespace checked {

std::optional<std::uint64_t> add(std::uint64_t a, std::uint64_t b) noexcept;
std::optional<std::uint64_t> multiply(std::uint64_t a, std::uint64_t b) noexcept;
std::optional<std::uint64_t> pageCount(std::uint64_t count, std::uint64_t capacity) noexcept;
bool range(std::uint64_t offset, std::uint64_t length, std::uint64_t fileSize) noexcept;
std::optional<qint64> fileOffset(std::uint64_t value) noexcept;
std::optional<std::size_t> memorySize(std::uint64_t value) noexcept;

} // namespace checked
} // namespace cadcontour::cache
