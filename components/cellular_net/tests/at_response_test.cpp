#include "../at_response.hpp"

#include <cassert>
#include <initializer_list>
#include <string>
#include <vector>

using cellular_net::run_at;
using esp_modem::command_result;
using std::string_view;

struct UartDte {
    std::initializer_list<string_view> chunks;

    template<typename Callback>
    command_result command(const char *, Callback callback, uint32_t)
    {
        std::string response;
        for (string_view chunk : chunks) {
            response += chunk;
            // esp_modem invokes the command callback only for UART reads
            // containing LF and passes the accumulated response.
            if (chunk.find('\n') != string_view::npos) {
                auto result = callback(reinterpret_cast<uint8_t *>(response.data()), response.size());
                if (result != command_result::TIMEOUT) {
                    return result;
                }
            }
        }
        return command_result::TIMEOUT;
    }
};

static void expect_lines(std::initializer_list<string_view> chunks,
                         std::initializer_list<string_view> expected)
{
    UartDte dte{chunks};
    std::vector<std::string> lines;
    auto result = run_at(dte, "AT+CGACT?\r", 3000, [&](string_view line) {
        lines.emplace_back(line);
        return line == "OK" ? command_result::OK : command_result::TIMEOUT;
    });
    assert(result == command_result::OK);
    assert(lines == std::vector<std::string>(expected.begin(), expected.end()));
}

int main()
{
    expect_lines({"+CGACT: 8,1\r\nOK\r\n"}, {"+CGACT: 8,1", "OK"});
    expect_lines({"+CGA", "CT: 8,", "1\r", "\nO", "K\r", "\n"},
                 {"+CGACT: 8,1", "OK"});
    expect_lines({"+CEREG: 1\r\n+CG", "ACT: 8,0\r\n+CMTI: \"SM\",2\r", "\n", "OK\r\n"},
                 {"+CEREG: 1", "+CGACT: 8,0", "+CMTI: \"SM\",2", "OK"});

    const auto handle_final = [](string_view line) {
        return line == "CONNECT 115200" || line == "OK"
                   ? command_result::OK : command_result::TIMEOUT;
    };
    UartDte dial{{"+CEREG: 1\r\nCON", "NECT 115200\r", "\n"}};
    assert(run_at(dial, "AT+CGDATA\r", 3000, handle_final) == command_result::OK);
    UartDte error{{"+CME ER", "ROR: 30\r", "\nOK\r\n"}};
    assert(run_at(error, "AT\r", 3000, handle_final) == command_result::FAIL);

    std::string long_line(cellular_net::at_line_capacity + 1, 'x');
    UartDte overlong{{long_line, "\r\nOK\r\n"}};
    assert(run_at(overlong, "AT\r", 3000, handle_final) == command_result::FAIL);
    const std::string tail = "RING\r\n" + long_line;
    UartDte overlong_tail{{tail, "OK\r\n"}};
    assert(run_at(overlong_tail, "AT\r", 3000, handle_final) == command_result::FAIL);

    std::string full;
    while (full.size() < cellular_net::at_response_capacity) {
        full += "RING!!\r\n";
    }
    full.resize(cellular_net::at_response_capacity);
    full.back() = '\n';
    UartDte full_response{{full}};
    assert(run_at(full_response, "AT\r", 3000, handle_final) == command_result::FAIL);
    full.replace(full.size() - 8, 8, "\r\n\r\nOK\r\n");
    UartDte exact_fit{{full}};
    assert(run_at(exact_fit, "AT\r", 3000, handle_final) == command_result::OK);

    UartDte unsolicited_only{{"RING\r\n", "+CEREG: 1\r\n"}};
    assert(run_at(unsolicited_only, "AT\r", 3000, handle_final) == command_result::TIMEOUT);
    UartDte fresh{{"OK\r\n"}};
    assert(run_at(fresh, "AT\r", 3000, handle_final) == command_result::OK);
}
