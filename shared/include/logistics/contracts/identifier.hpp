#pragma once

#include <cstddef>
#include <string_view>

namespace logistics::contracts {

[[nodiscard]] constexpr bool IsHexDigit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

[[nodiscard]] constexpr bool IsValidUuid(std::string_view value) noexcept {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index != 8U && index != 13U && index != 18U && index != 23U && !IsHexDigit(value[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace logistics::contracts
