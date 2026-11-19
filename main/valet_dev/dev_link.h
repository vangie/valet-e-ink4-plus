#pragma once

// Valet dev link: "@DEV {json}" commands on the USB serial console, compiled in
// only with CONFIG_VALET_DEV_LINK. Driven by tools/dev/valetdev.py.
//
// Coordinates are portrait panel pixels (684 x 1216, origin top-left), the same
// as a screenshot rotated upright. The three cover keys sit below the panel.

/** @brief Handle one command line; `json` is the text after "@DEV ". */
void ValetDevLinkHandle(const char* json);
