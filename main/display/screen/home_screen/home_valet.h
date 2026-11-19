#pragma once
#include <lvgl.h>
// Valet first page; application pages continue using the existing home renderer.
void HomeValet_Build(lv_obj_t* screen, lv_coord_t status_height);
