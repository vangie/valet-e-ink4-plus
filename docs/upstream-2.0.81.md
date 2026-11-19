# 上游2.0.81与首次设备接入核对

核对日期：2026-11-19。尚未刷写、擦除或备份Flash，也未读取eFuse安全状态。

## 来源状态

- 官方烧录平台：<https://burner.cloudzao.com/zh/registry>，已发布2.0.81，平台记录发布时间2026-11-18 21:13:59 +08:00。
- 平台只读API：<https://admin.metalio.world/xiaozhi/api/firmwares?page=1&pageSize=100>。
- GitHub：<https://github.com/CloudZao/Metalio-E-INK4-Plus>，main仍为`2e8501095eaf1980be0d6d8b7005d0ffeae505f7`（v2.0.80）；没有2.0.81分支、tag或GitHub Release。
- Gitee：<https://gitee.com/cloudzao/metalio-e-ink4-plus>，HEAD/main同为`2e8501095eaf1980be0d6d8b7005d0ffeae505f7`。
- 已fetch upstream/main并确认其为本地HEAD的祖先，当前没有可合并的新版源码。没有将本地PROJECT_VER改成2.0.81。

## 设备只读观察

`/dev/cu.usbmodem1101`串口启动日志确认：ESP32-S31、16MB、Metalio-E-Ink4-Plus、App version 2.0.81，编译时间2026-11-18 16:40:12；从ota_1（0x580000）启动。日志中的全部分区offset/size与仓库partitions.csv相符。另一端口为CH340串口，未确认归属，不应自动刷写它。

读取期间出现USB断连/重新枚举，日志含CHIP_USB_UART_RESET；这不能判断为固件自身崩溃，先检查USB切换/串口打开控制线行为，避免反复打开端口。没有写Flash。

## 首次开发固件上机前

1. 优先等官方公布2.0.81源码，再按提交做三方合并，重跑构建/回归；仅取得二进制不能视为源码同步。
2. 备份实物完整16MB Flash、NVS和OTA选择数据，校验备份后保存于仓库外私有目录；备份可能含Wi-Fi、设备身份、授权信息，不能提交。
3. 检查设备实际安全启动/加密状态；本地sdkconfig关闭不等同于实物eFuse状态。
4. 检查FT6336U面板与当前触控版本。上游启动路径默认启用触摸芯片固件更新，第一次应用固件测试应显式关闭CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT，避免同时升级独立触控芯片。主Flash备份不能恢复独立触控固件。
5. 优先保持现有bootloader、分区表、NVS、官方在用ota_1、资源和字库，评估仅写非活动app槽的测试和回退方法；不能直接运行包含bootloader/otadata/resources/font_data的整包flash命令。
6. 官方云配置在公开源码中缺失的问题仍存在，保留原NVS并不能保证开发固件继承官方订阅。

## 官方2.0.81更新说明（平台原文）

2.0.81
新增：
1、自动测试增加VCOM显示及查询、增加UI测试入口、显示蓝牙芯片版本和SD卡容量、优化自动测试样式，并完善Wi-Fi测试弹窗的关闭和返回处理。
2、设置-测试增加关机、重启按钮。

优化：
1、国内版云服务隐藏小智和网易 AI 入口，国际版云服务隐藏小智和百问 AI 入口。
2、开机未读取到 VCOM 时，屏幕设置提供黑色深度选择档位；读取成功时保持现有 VCOM 设置方式。

修复：
1、修复像素视界主题下刚开机时最近阅读处没有显示已保存的阅读进度的问题。
2、修复封面及图片加载期间的屏幕反复刷新问题。
3、修复书架翻页时先出旧画面才更新新画面的问题。
4、修复开机时概率连续两次推白的问题。
5、修复横屏待机或关机时透明壁纸随界面旋转的问题。
6、修复使用 TTF/OTF 字体时快速进入并退出待机后，阅读页面右侧文字显示不完整的问题。

## 同步准备与原型验证

同步检查确认公开main仍为2e850109，prepare未创建空合并。隔离Git测试五项通过，覆盖完整合并、无更新、脏树、冲突及上游改写历史。两个Tab新增交互、四种手机宽度触控、Chromium/WebKit七颗物理按键回归通过。2.0.81硬件修复仍待对应源码及设备验收。本次未刷机。
