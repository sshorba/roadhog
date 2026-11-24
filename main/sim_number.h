#pragma once

#include <stdbool.h>

enum { SIM_NUMBER_CAPACITY = 17 };
bool sim_number_parse(const char *response, char out[SIM_NUMBER_CAPACITY]);
