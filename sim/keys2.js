// 屏下三颗键（固件里叫 vk_home、vk_prev、vk_next）和电源键。
function blocked() {
  return asleep() || state.standby;
}

function menuClick() {
  if (blocked()) return;
  if (state.screen === "home") return log("菜单：已经在首页。");
  if (state.screen === "reader" && state.sheet) {
    state.sheet = false;
    log("菜单：收起阅读卡片。");
    return render();
  }
  if (state.screen === "reader") log("菜单：离开正文，回书的详情。");
  back();
}

function menuLong() {
  if (blocked() || state.screen === "home") return;
  log("长按菜单：直接回首页。");
  goHome();
}

function chin(dir) {
  if (blocked()) return;
  const s = state.screen;
  if (s === "reader") {
    if (!state.sheet) return void turnPage(dir);
    if (dir < 0) {
      state.sheet = false;
      log("上一页：收起阅读卡片。");
      render();
    }
    return;
  }
  if (s === "ai") {
    if (state.answered && !state.live) {
      const n = state.answerPage + dir;
      if (n >= 0 && n < answerFor().length) {
        state.answerPage = n;
        flash();
        return render();
      }
    }
    if (dir < 0) back();
    return;
  }
  if (s === "home") return log("首页只有这一屏，不翻。");
  if (dir < 0) back();
}

function chinLong(dir) {
  if (blocked()) return;
  if (readingActive()) return startRepeat(dir);
  chin(dir);
}

let wasLive = false;
function powerClick() {
  if (state.booting) return;
  if (state.off) return powerOn();
  if (state.standby) {
    state.standby = false;
    log("电源短按：醒来，还在刚才那一屏。");
    flash();
    render();
    if (state.screen === "ai" && wasLive) startLive();
    return;
  }
  wasLive = state.live;
  if (state.screen === "ai") stopSession();
  state.standby = true;
  log("电源短按：待机。");
  render();
}

function powerLong() {
  if (state.off || state.booting) return;
  state.bootTo = state.screen === "reader" ? "reader" : "home";
  leave(state.screen);
  stack.length = 0;
  state.standby = false;
  state.off = true;
  log(`长按电源 3 秒：关机。屏上留着按键说明。${state.bootTo === "reader" ? `记住第 ${state.page + 1} 页。` : ""}`);
  render();
}

function powerOn() {
  state.off = false;
  state.booting = true;
  render();
  later("boot", 900, () => {
    state.booting = false;
    state.screen = state.bootTo;
    if (state.screen === "reader") stack.push("home", "detail");
    log(state.screen === "reader" ? `开机，回到第 ${state.page + 1} 页。` : "开机，回到首页。");
    flash();
    render();
  });
}

function reboot() {
  state.provider = state.pendingProvider;
  stack.length = 0;
  state.screen = "home";
  state.booting = true;
  render();
  later("boot", 900, () => {
    state.booting = false;
    log(`重启完成。云服务是「${PROVIDERS[state.provider]}」，第 ${state.page + 1} 页还记着。`);
    flash();
    render();
  });
}
