// 按键行为照官方固件 2.0.82：boot_key_handler.cc、lv_adapter_epdiy.cc、book_vk.cc、assistant_screen.cc。
const LONG_MS = 500; // TOUCH_VK_LONG_MS、kBootLongPressMs
const POWER_LONG_MS = 3000; // POWER_LONG_PRESS_MS

function readingActive() {
  return state.screen === "reader" && !state.sheet;
}
function asleep() {
  return state.off || state.booting;
}

// 正文翻页。第一页再往前回书的详情，和固件 vk_prev 一样。
function turnPage(delta, repeating = false) {
  const next = state.page + delta;
  if (next < 0) {
    if (!repeating) {
      log("第一页再往前：返回上一级。");
      back();
    }
    return false;
  }
  if (next >= pages.length) {
    if (!repeating) log("假书只有 3 页，这是最后一页。");
    return false;
  }
  state.page = next;
  log(`翻到第 ${state.page + 1} 页。${state.float ? "悬窗跟着到这一页。" : ""}`);
  if (isV2() && state.float) v2FloatFollowPage();
  flash();
  render();
  return true;
}

let repeatTimer = null;
function startRepeat(delta) {
  stopRepeat();
  if (!turnPage(delta, true)) return;
  repeatTimer = setInterval(() => {
    if (!turnPage(delta, true)) stopRepeat();
  }, 420);
}
function stopRepeat() {
  clearInterval(repeatTimer);
  repeatTimer = null;
}

// AI 唤醒（固件里叫 BOOT）
function aiClick() {
  if (asleep()) return;
  if (state.standby) return log("待机时短按 AI 唤醒不醒，长按才醒来进百问。");
  if (state.screen === "reader" && state.float) {
    // 悬窗里：AI 在说就打断，没在说就收起悬窗
    if (state.speaking) return state.live ? interruptLive() : abortSpeaking();
    return void v2CloseFloat("短按 AI 唤醒");
  }
  if (state.screen === "reader") {
    state.sheet = !state.sheet;
    log(state.sheet ? "短按 AI 唤醒：弹出阅读卡片。" : "短按 AI 唤醒：收起阅读卡片。");
    return render();
  }
  if (state.screen === "ai") return state.live ? interruptLive() : abortSpeaking();
  log(`短按 AI 唤醒：${names()[state.screen]}不处理短按。`);
}

function aiLong() {
  if (asleep()) return;
  const fromStandby = state.standby;
  state.standby = false;
  if (state.screen === "ai") {
    if (fromStandby) log("待机时长按 AI 唤醒：醒来回到百问。");
    if (fromStandby && wasLive) startLive();
    if (state.live) {
      render();
      return log("通话里一直在听，不用按住。");
    }
    log("长按 AI 唤醒：聆听中，松开发送。");
    return startListening();
  }
  // 新设计：正文里长按 AI 唤醒在悬窗里问，书页不动
  if (isV2() && state.screen === "reader" && state.provider === "agent" && agentReady()) {
    v2OpenFloat(false);
    if (state.live) return log("通话里一直在听，不用按住。");
    if (held("ai")) { log("长按 AI 唤醒：在悬窗里听，松开发送。"); startListening(); }
    return;
  }
  // 新设计：任何一屏长按 AI 唤醒都能问。官方只有首页、阅读首页和正文。
  if (!fromStandby && !isV2() && !["home", "detail", "reader"].includes(state.screen)) {
    return log(`长按 AI 唤醒：${names()[state.screen]}不进百问。`);
  }
  if (fromStandby) log("待机时长按 AI 唤醒：醒来直接进百问。");
  go("ai");
  if (!state.live && held("ai")) {
    log("没松手，直接开始听。松开发送。");
    startListening();
  }
}

function aiUp() {
  if ((state.screen === "ai" || state.float) && state.listening && !state.live) stopListeningAndSend();
}

// 加音量、减音量：正文里翻页（加 = 上一页），别处调音量。
function volume(dir) {
  if (asleep() || state.standby) return;
  if (readingActive()) return void turnPage(-dir);
  state.volume = Math.max(0, Math.min(100, state.volume + dir * 10));
  toast(`音量 ${state.volume}`);
  render();
}
function volumeLong(dir) {
  if (asleep() || state.standby) return;
  if (readingActive()) return startRepeat(-dir);
  state.volume = dir > 0 ? 100 : 0;
  render();
  toast(dir > 0 ? "最大音量" : "已静音");
}
