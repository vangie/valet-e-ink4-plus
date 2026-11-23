#pragma once

#include <lvgl.h>

struct EpdStatusBar;

// The status bar keeps its labels (the display adapter writes their text), but they are drawn as icons:
//  - network: the board's network icon text, plus kStatusProxyMark when the proxy is in force
//             → Wi-Fi arcs / 4G bars / plane, and a shield next to it;
//  - battery: "57", or "57+" while charging → outlined battery, level filled black, digits inside
//             (white over the fill, black over the empty part), no %;
//  - mute:    any text → crossed-out speaker; empty → the label is hidden.

constexpr char kStatusProxyMark = 'P';

/** @brief Turn the status bar's labels into icon views. Called by ScreenCreateStatusBar. */
void StatusBarIconsAttach(const EpdStatusBar& bar);
/** @brief Battery label text for a level (0..100) and charging state. */
void StatusBarBatteryText(char* out, size_t size, int level, bool charging);
/** @brief Show or hide the mute icon (sets the label's text and hidden flag). */
void StatusBarSetMuted(lv_obj_t* mute_label, bool muted);
