#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sim_number.h"

int main(void)
{
    char number[SIM_NUMBER_CAPACITY];
    assert(sim_number_parse("+CNUM: \"\",\"1234567890\",129\n", number));
    assert(!strcmp(number, "1234567890"));
    assert(sim_number_parse("+CNUM: \"Owner, line\", \"+123456789012345\", 145\n", number));
    assert(!strcmp(number, "+123456789012345"));
    assert(sim_number_parse("+CNUM: ,\"+1234\",145", number));
    const char *invalid[] = {
        "", "ERROR", "+CNUM: \"\",\"\",129",
        "+CNUM: \"\",\"+01234\",145",
        "+CNUM: \"\",\"+1234567890123456\",145",
        "+CNUM: \"\",\"+1234\",145,<html>",
        "+CNUM: \"\",\"+1234\",129",
        "+CNUM: \"\",\"+1234\",145\n+CNUM: \"\",\"+5678\",145\n",
        "+CNUM: \"\",\"123\\\"4\",129",
        "+CNUM: \"unclosed,\"+1234\",145",
        "+CNUM: \"\",\"+1234\",",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        assert(!sim_number_parse(invalid[i], number) && number[0] == 0);
    }
    puts("SIM number parsing fixtures passed");
}
