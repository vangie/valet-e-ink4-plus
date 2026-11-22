#pragma once

#include "lvgl.h"
#include <string>

// 百问 AI 页面：管理会话内容、PPT 状态和按键语义。
class AssistantScreen {
public:
    /** @brief 创建百问页 */
    static lv_obj_t* Create();
    static void RequestLive();

    /** @brief 当前服务的应用名称；首页使用 compact 英文缩写 */
    static const char* DisplayName(bool compact = false);

    /** @brief 当前页面是否在前台；后台消息会被忽略 */
    static bool IsActive();

    /** @brief 在 LVGL 线程暂停或恢复前台保网和语音连接；唤醒不自动开始录音 */
    static void SetStandby(bool standby);

    /** @brief PTT 是否仍保持按下；用于统一监听和松手判定 */
    static bool IsPttHeld();

    /** @brief 状态栏是否显示录音波形 */
    static bool IsPttWaveVisible();

    /** @brief 同步当前 PTT/监听状态到状态栏 */
    static void SyncPttOverlay();
    /** @brief 在 LVGL 任务中为本轮录音固定待发送照片 */
    static bool BeginPhotoListening(uint32_t turn);
    /** @brief 松手后上传本轮照片，再通知语音任务结束录音 */
    static bool EndPhotoListening(uint32_t turn);
    /** @brief 将单图提问的连接结果交给页面上传流程 */
    static bool UploadPhotoQuestion(uint32_t turn, bool ready);
    /** @brief 显示图片提问错误，并恢复当前照片供重试 */
    static void PhotoQuestionError(const std::string& image_id, bool reupload, bool disabled);
    /** @brief 图片回答结束后释放重试用原图 */
    static void FinishPhotoQuestion();

    /** @brief 异步打开页面；已在前台则忽略 */
    static void RequestOpen();

    /** @brief 向会话流追加消息；须在持 LVGL 锁时调用 */
    static void AddMessage(const char* role, const char* content);
    static void FinishYunxinSpeech();
    static void AddYunxinSpeech(const char* text);
    static void FlushYunxinSpeech();
    // 通话（自建）：助理这段回答到目前为止要显示的文字。第一次追加一条「AI：」，之后原地替换这一条
    // （只重排最后一页）；done 表示这段回答结束，下一次再显示就是新的一条。
    static void ShowLiveReply(const char* text, bool done);
    // 助理那边结束了通话（用户说「挂掉吧」等）：和按「挂断」一样回到提问页。
    static void EndLiveCall();

    /** @brief 设置情绪表情（当前可为空操作） */
    static void SetEmotion(const char* emotion);
};
