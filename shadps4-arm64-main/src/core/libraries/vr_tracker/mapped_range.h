// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace Libraries::VrTracker {
struct MappedSpan {
    std::uintptr_t start;
    std::uintptr_t end;
    bool committed;
    std::int32_t memory_type;
    std::int32_t protection;
};

// Virtual contiguity is sufficient: titles may assemble a tracker allocation
// from separate direct-memory mappings with noncontiguous physical backing.
template <class Query>
bool ValidateMappedRange(std::uintptr_t address, std::uintptr_t size, Query&& query) {
    if (!address || !size || size > std::numeric_limits<std::uintptr_t>::max() - address)
        return false;
    const auto end = address + size;
    std::optional<MappedSpan> first;
    while (address < end) {
        const auto span = query(address);
        if (!span || !span->committed || span->start > address || span->end <= address)
            return false;
        if (first && (span->memory_type != first->memory_type ||
                      span->protection != first->protection))
            return false;
        if (!first)
            first = span;
        address = span->end;
    }
    return true;
}
} // namespace Libraries::VrTracker
