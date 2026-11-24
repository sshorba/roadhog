#include "sim_number.h"

#include <string.h>

static const char *spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

static bool number_line(const char *p, char out[SIM_NUMBER_CAPACITY])
{
    if (strncmp(p, "+CNUM:", 6)) {
        return false;
    }
    p = spaces(p + 6);
    if (*p == '"') {
        p = strchr(p + 1, '"');
        if (!p) {
            return false;
        }
        p = spaces(p + 1);
    }
    if (*p != ',') {
        return false;
    }
    p = spaces(p + 1);
    if (*p++ != '"') {
        return false;
    }
    const char *number = p;
    bool international = *p == '+';
    p += international;
    size_t digits = strspn(p, "0123456789");
    if (digits < 3 || digits > 15 || (international && *p == '0') || p[digits] != '"') {
        return false;
    }
    p = spaces(p + digits + 1);
    if (*p != ',') {
        return false;
    }
    p = spaces(p + 1);
    const char *suffix = international ? "145" : "129";
    if (strncmp(p, suffix, 3) || *spaces(p + 3)) {
        return false;
    }
    size_t length = digits + international;
    memcpy(out, number, length);
    out[length] = '\0';
    return true;
}

bool sim_number_parse(const char *response, char out[SIM_NUMBER_CAPACITY])
{
    out[0] = '\0';
    size_t length = strcspn(response, "\n");
    // Multiple entries can identify different services. Show unavailable
    // rather than guessing which one is the device's SMS destination.
    if (!length || length >= 384 || (response[length] && response[length + 1])) {
        return false;
    }
    char line[384];
    memcpy(line, response, length);
    line[length] = '\0';
    return number_line(line, out);
}
