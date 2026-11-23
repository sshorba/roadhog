#pragma once

#include <stdbool.h>

// Call once after essential startup checks or before a fatal startup return.
void ota_boot_health_finish(bool healthy);
