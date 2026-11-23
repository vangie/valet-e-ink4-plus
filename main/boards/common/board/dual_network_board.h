#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <driver/gpio.h>

enum class NetworkType {
    WIFI = 0,
    CELLULAR = 1
};

// 「怎么上网」选的：只用 Wi-Fi、只用 4G，或自动（有存过的 Wi-Fi 就用，没有就 4G）
enum class NetworkMode {
    WIFI = 0,
    CELLULAR = 1,
    AUTO = 2
};

// 双网络板卡：Wi-Fi 和 4G 两个板卡对象都常驻（用到时才创建，之后不销毁），
// 同一时间只有一个在用。切换时停掉一个的射频、拉起另一个，不重启。
// 两个对象都不销毁：别的任务手里的 GetCurrentBoard() 引用即使过期也不会悬空。
class DualNetworkBoard : public Board {
private:
    std::unique_ptr<Board> wifi_board_;
    std::unique_ptr<Board> cell_board_;
    std::atomic<Board*> active_{nullptr};
    std::atomic<NetworkType> network_type_{NetworkType::CELLULAR};
    std::atomic<NetworkMode> network_mode_{NetworkMode::AUTO};
    NetworkEventCallback network_event_callback_;
    std::mutex switch_mu_;

    // Cellular(NT26) pin configuration
    gpio_num_t cellular_tx_pin_;
    gpio_num_t cellular_rx_pin_;
    gpio_num_t cellular_dtr_pin_;
    gpio_num_t cellular_ri_pin_;

    // 保存网络类型到Settings
    void SaveNetworkTypeToSettings(NetworkType type);
    // 取（必要时创建）某种网络的板卡对象
    Board& BoardFor(NetworkType type);
    Board& Active() const { return *active_.load(std::memory_order_acquire); }

public:
    // Explicit constructor with SRDY/RI pin.
    DualNetworkBoard(gpio_num_t cellular_tx_pin,
        gpio_num_t cellular_rx_pin,
        gpio_num_t cellular_dtr_pin,
        gpio_num_t cellular_ri_pin,
        int32_t default_net_type);

    virtual ~DualNetworkBoard() = default;

    // 从 Settings 加载网络类型（namespace "network", key "type"：0=WiFi, 1=4G）
    static NetworkType LoadNetworkTypeFromSettings(int32_t default_net_type);

    /** @brief 切到指定网络，不重启：停掉当前网络的射频，换上另一个并在后台连网。
     *         正在用的连接会断开（上层自己重连）。会写 NVS，须在内部内存栈的任务里调用。
     *         已是目标类型返回 true；传输热点占着 Wi-Fi 射频时不切，返回 false。 */
    bool SwitchToNetworkType(NetworkType type);
    /** @brief 切换完成后调用（例如按新网络重新套用代理）。只设一次。 */
    using SwitchedHook = void (*)(NetworkType type);
    static void SetSwitchedHook(SwitchedHook hook);
    /** @brief 自动模式下用 4G 却还没连上（多半刚醒）时调用：可以先看看有没有存过的 Wi-Fi。不得阻塞。 */
    using WakeHook = void (*)();
    static void SetWakeHook(WakeHook hook);

    NetworkMode GetNetworkMode() const { return network_mode_.load(std::memory_order_acquire); }
    /** @brief 改「怎么上网」并保存。选 Wi-Fi / 4G 时马上切过去（不重启）；选自动时不动当前网络。
     *         会写 NVS、可能停 Wi-Fi 驱动：须在内部内存栈的任务里调用。 */
    bool SetNetworkMode(NetworkMode mode);

    // 获取当前网络类型
    NetworkType GetNetworkType() const { return network_type_.load(std::memory_order_acquire); }
    /** @brief 获取当前网络类型对应的不可用提示 */
    const char* GetNetworkErrorText(const char* fallback) override { return Active().GetNetworkErrorText(fallback); }

    // 获取当前活动的板卡引用
    Board& GetCurrentBoard() const { return Active(); }

    AudioCodec* GetAudioCodec() override { return nullptr; }
    const char* GetNetworkBusyStatusText() override { return Active().GetNetworkBusyStatusText(); }
    void SetNetworkEventCallback(NetworkEventCallback callback) override;
    // 重写Board接口
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual bool EnsureNetworkReady(int timeout_ms = 30000) override;
    virtual void RefreshNetworkWaitDeadline(int timeout_ms = 30000) override;
    virtual void PauseNetworkIfNotReady() override;
    virtual bool IsNetworkReady() override;
    virtual bool IsWifiConfigMode() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif // DUAL_NETWORK_BOARD_H
