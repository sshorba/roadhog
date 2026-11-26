#pragma once

#include <cstdint>
#include <string_view>

namespace cellular_net {

enum class maintenance_action { unrelated, invalid, inspect, arm, confirm };

inline maintenance_action parse_maintenance(std::string_view line)
{
    if (line == "modem inspect") {
        return maintenance_action::inspect;
    }
    if (line == "modem cleanup 8") {
        return maintenance_action::arm;
    }
    constexpr std::string_view confirm = "modem cleanup 8 confirm ";
    if (line.starts_with(confirm)) {
        auto token = line.substr(confirm.size());
        if (token.size() == 8 && token.find_first_not_of("0123456789abcdef") == std::string_view::npos) {
            return maintenance_action::confirm;
        }
    }
    return line.starts_with("modem") ? maintenance_action::invalid : maintenance_action::unrelated;
}

struct cleanup_confirmation {
    int64_t expires = 0;
    char token[9] = {};

    bool consume(std::string_view line, int64_t now)
    {
        bool valid = expires > now && line.ends_with(token);
        expires = 0;
        return valid;
    }
};

} // namespace cellular_net
