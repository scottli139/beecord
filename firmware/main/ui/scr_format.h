#pragma once

#include "device_model.h"

void scr_format_create(void);
void scr_format_render(const device_snapshot_t *snapshot);
void scr_format_debug_click(bool confirm);
