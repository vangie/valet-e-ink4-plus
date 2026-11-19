#pragma once
#include <lvgl.h>
// Valet home: the first page (clock, Ask / Call, reading, to-do) and the app
// pages behind it. The pixel theme keeps its own renderer.
constexpr int kHomeValetAppsPerPage = 8;
// Icon marker for the shelf entry: there is no shelf image in the resources, so the grid draws it.
constexpr const char* kHomeValetShelfIcon = "valet:shelf";
void HomeValet_Build(lv_obj_t* screen, lv_coord_t status_height);
void HomeValet_BuildApps(lv_obj_t* screen, lv_coord_t status_height, int card_style);
