#pragma once

// 「怎么上网」选自动时由这里决定用 Wi-Fi 还是 4G，切换不重启（DualNetworkBoard::SwitchToNetworkType）。
// - 在 Wi-Fi 上：WifiStation 一次扫描找不到存过的网络，或存过的都连不上 → 换 4G（通话中也换：那时 Wi-Fi 已经断了）。
// - 在 4G 上：醒着、空闲时每隔几分钟扫一次 Wi-Fi；刚醒要用网时（4G 还没连上）也扫一次。
//   扫到存过的网络、信号够 → 换回 Wi-Fi。提问、通话、下载中不换。
// 扫描时临时初始化 Wi-Fi 驱动，扫完整个关掉，4G 一直在用。

namespace valet::autolink {

/** @brief 启动后台任务并挂上 WifiStation / DualNetworkBoard 的通知。开机时调用一次。 */
void Init();
/** @brief Test builds: scan for saved Wi-Fi now (on 4G), skipping the wait between scans. */
void ProbeNow();

}  // namespace valet::autolink
