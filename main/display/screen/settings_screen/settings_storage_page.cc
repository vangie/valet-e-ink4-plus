#include "settings_storage_page.h"

#include "SdCardManager.hpp"
#include "application.h"
#include "assets/lang_config.h"
#include "cloud_screen/cloud_screen.h"
#include "screen_common.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "settings_common.h"
#include "usb_virtual_disk.h"

#include <cstdio>
#include <cstring>

#include <esp_vfs_fat.h>

namespace {
lv_obj_t* s_card = nullptr;
uint32_t s_generation = 0;

const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}

struct Room { uint32_t generation; bool ok; uint64_t total; uint64_t free; };
void ShowRoom(void* context) {
    auto* room = static_cast<Room*>(context);
    if (s_card && room->generation == s_generation) {
        char detail[96];
        const double gb = 1024.0 * 1024.0 * 1024.0;
        if (room->ok) std::snprintf(detail, sizeof(detail), Text("还剩 %.1f GB，共 %.1f GB", "%.1f GB free of %.1f GB"),
                                    static_cast<double>(room->free) / gb, static_cast<double>(room->total) / gb);
        else std::snprintf(detail, sizeof(detail), "%s", Text("容量没读出来", "Could not read the capacity"));
        SettingsValetCardShow(s_card, Text("SD 卡 · 已插入", "SD card · inserted"), detail, true, true);
    }
    delete room;
}
}  // namespace

const char* SettingsStoragePage_Summary() {
    if (UsbVirtualDisk::GetInstance().IsSdExportedToHost()) return Text("电脑正在当 U 盘用", "In use by a computer as a USB disk");
    return SdCardManager::GetInstance().IsMounted() ? Text("SD 卡已插入", "SD card inserted") : Text("没有 SD 卡", "No SD card");
}

void SettingsStoragePage_Reset() {
    s_card = nullptr;
    ++s_generation;   // a capacity reading still under way will not write into the next page
}

void SettingsStoragePage_Build(lv_obj_t* page) {
    ++s_generation;
    lv_obj_set_style_pad_row(page, 0, 0);
    s_card = SettingsValetCardSlot(page);
    const bool exported = UsbVirtualDisk::GetInstance().IsSdExportedToHost();
    const bool mounted = SdCardManager::GetInstance().IsMounted();
    if (exported) {
        SettingsValetCardShow(s_card, Text("SD 卡 · 电脑在用", "SD card · in use by a computer"),
                              Text("电脑上弹出 U 盘后，这里才能用卡", "Eject it on the computer to use the card here"), false);
    } else if (!mounted) {
        SettingsValetCardShow(s_card, Text("没有 SD 卡", "No SD card"),
                              Text("书、壁纸、扩展字库都放在卡上", "Books, wallpapers and the font pack live on the card"), false);
    } else {
        SettingsValetCardShow(s_card, Text("SD 卡 · 已插入", "SD card · inserted"), Text("正在读容量…", "Reading the capacity…"), true, true);
        // Reading the capacity touches the card: do it on the storage worker, not on the UI task.
        const uint32_t generation = s_generation;
        Application::GetInstance().Schedule([generation]() {
            auto* room = new Room{generation, false, 0, 0};
            {
                sd_access::Transaction transaction;
                room->ok = transaction && esp_vfs_fat_info(SD_MOUNT_POINT, &room->total, &room->free) == ESP_OK;
            }
            if (!ScreenLvAsync(ShowRoom, room)) delete room;
        }, Application::TaskDomain::Storage);
    }
    SettingsValetGap(page, 26);
    // The USB disk is switched on and off in one place, the Transfer app, which also turns it off on leaving.
    SettingsValetRow(page, Text("当 U 盘用", "Use as a USB disk"),
                     Text("打开「传输」的 U 盘一栏，接上电脑拷文件", "Opens the USB disk tab of Transfer; copy files from a computer"),
                     [](lv_event_t*) { ScreenNavigateTo(CloudScreen::CreateOnUsb); }, 0, true);
    SettingsValetGap(page);
}
