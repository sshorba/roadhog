#pragma once

#include <string_view>

#include "cxx_include/esp_modem_types.hpp"

namespace cellular_net {

inline constexpr size_t at_response_capacity = 4096;
inline constexpr size_t at_line_capacity = 384;

template<typename Modem, typename LineHandler, typename ErrorHandler>
esp_modem::command_result run_at(Modem &modem, const char *command, uint32_t timeout_ms,
                                 LineHandler handle_line, ErrorHandler handle_error)
{
    using esp_modem::command_result;

    // esp_modem 2.1.0 UART DTE passes the full accumulated response each time.
    // The DTE retains partial lines. Keep our offset at the first unparsed
    // byte so a split line is processed once, when its newline arrives.
    size_t next_line = 0;
    return modem.command(command, [&](uint8_t *data, size_t size) {
        if (size < next_line || size > at_response_capacity) {
            return command_result::FAIL;
        }
        std::string_view response(reinterpret_cast<const char *>(data), size);
        for (;;) {
            size_t end = response.find('\n', next_line);
            if (end == std::string_view::npos) {
                // A full DTE buffer cannot receive the rest of this response.
                return size == at_response_capacity || size - next_line > at_line_capacity
                           ? command_result::FAIL : command_result::TIMEOUT;
            }
            std::string_view line = response.substr(next_line, end - next_line);
            next_line = end + 1;
            if (line.size() > at_line_capacity) {
                return command_result::FAIL;
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (line == "ERROR" || line.starts_with("+CME ERROR:") ||
                line.starts_with("+CMS ERROR:")) {
                handle_error(line);
                return command_result::FAIL;
            }
            command_result result = handle_line(line);
            if (result != command_result::TIMEOUT) {
                return result;
            }
        }
    }, timeout_ms);
}

template<typename Modem, typename LineHandler>
esp_modem::command_result run_at(Modem &modem, const char *command, uint32_t timeout_ms,
                                 LineHandler handle_line)
{
    return run_at(modem, command, timeout_ms, handle_line, [](std::string_view) {});
}

} // namespace cellular_net
