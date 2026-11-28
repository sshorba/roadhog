#pragma once

#include <stdbool.h>

bool ota_boot_health_pending(void);

// Call once after essential startup checks pass or their deadline expires.
void ota_boot_health_finish(bool healthy);
