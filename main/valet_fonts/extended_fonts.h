#pragma once

#include <cstdint>

// The extended font pack: 100k+ characters for the UI and for reading, kept on
// the SD card because it is far larger than the firmware. This module downloads
// it from the project's release page over Wi-Fi, checks it, and puts it where
// the font loader looks (SD_PATH_FONT_EXT). Nothing here runs on the UI task.
namespace valet::fonts {

enum class State : uint8_t {
    kMissing,      // not on the card
    kInstalled,    // file present with the expected size
    kDownloading,  // percent() is meaningful
    kVerifying,    // download complete, checking the file
    kFailed,       // message() says why; the partial file is kept for resuming
};

struct Status {
    State state = State::kMissing;
    int percent = 0;          // 0..100 while downloading
    uint64_t done_bytes = 0;
    char message[96] = {};    // failure reason, in the UI language
};

/** @brief Size of the pack in bytes, for the UI. */
uint64_t PackBytes();

/** @brief Current state. Cheap; reads no file. Call Refresh() to re-check the card. */
Status GetStatus();

/** @brief Re-check the card for the file, off the UI task. Safe to call repeatedly. */
void Refresh();

/**
 * @brief Start (or resume) the download.
 * @return false when it cannot start: no Wi-Fi, no card, already running.
 *         GetStatus().message then says which.
 */
bool Start();

/** @brief Stop a running download. The partial file stays for the next Start(). */
void Cancel();

}  // namespace valet::fonts
