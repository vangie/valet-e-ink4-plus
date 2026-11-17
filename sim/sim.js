// 把按键、屏幕和键盘接起来。
const nowEl = document.querySelector("#now");

function keyHelp() {
  if (state.off) return [["电源", "短按开机"]];
  if (state.booting) return [["", "开机中"]];
  if (state.standby) return [["AI 唤醒", "长按：醒来进百问"], ["电源", "短按醒来；长按 3 秒关机"], ["其他键", "不响应"]];
  const s = state.screen;
  const live = s === "ai" && state.provider === "agent" && state.live;
  let ai = "不响应";
  if (s === "home" || s === "detail") ai = "长按进百问，不松手直接开始听";
  if (s === "reader") ai = `短按${state.sheet ? "收起" : "弹出"}阅读卡片；长按带这一页进百问`;
  if (s === "ai") ai = live ? "短按打断 AI 说话" : "按住说话，松开发送；短按打断回答";
  const vol = readingActive() ? "加 = 上一页，减 = 下一页，长按连翻" : "调音量，长按到最大或静音";
  let menu = "返回；长按回首页";
  if (s === "home") menu = "已在首页";
  if (s === "reader") menu = state.sheet ? "收起卡片" : "回书的详情；长按回首页";
  if (s === "ai" && state.fromPage !== null) menu = `回第 ${state.fromPage + 1} 页；长按回首页`;
  let gray = "左键返回";
  if (s === "home") gray = "首页只有一屏";
  if (s === "reader") gray = state.sheet ? "左键收起卡片" : "翻页，长按连翻";
  if (s === "ai") gray = live ? "左键返回" : "翻回答；第一页再往前就返回";
  return [["AI 唤醒", ai], ["加 / 减音量", vol], ["电源", "短按待机；长按 3 秒关机"], ["菜单（橙点）", menu], ["两颗灰键", gray]];
}

function screenHtml() {
  if (state.off) return `<img class="full" src="assets/bg_shutdown_zh.png" alt="关机画面：按键说明" />`;
  if (state.booting) return `<div class="boot">Metalio</div>`;
  if (state.standby) return standbyScreen();
  return { home, detail, reader, ai, settings, wallpaper, tasks, transfer }[state.screen]();
}

function render() {
  let html = screenHtml();
  if (state.toast && !state.off && !state.booting) html += `<div class="toast">${esc(state.toast)}</div>`;
  panel.innerHTML = html;
  const where = state.off ? "关机" : state.booting ? "开机中" : state.standby ? "待机" : NAMES[state.screen];
  nowEl.innerHTML = `<p>现在在「${where}」，按键是：</p><dl>${keyHelp()
    .map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("")}</dl>`;
}

// 物理键：按下开始计时，到点算长按；没到点松手算短按。
const KEYS = {
  ai: { click: aiClick, long: aiLong, up: aiUp },
  "vol-up": { click: () => volume(1), long: () => volumeLong(1), up: stopRepeat },
  "vol-down": { click: () => volume(-1), long: () => volumeLong(-1), up: stopRepeat },
  power: { click: powerClick, long: powerLong, ms: POWER_LONG_MS },
  menu: { click: menuClick, long: menuLong },
  prev: { click: () => chin(-1), long: () => chinLong(-1), up: stopRepeat },
  next: { click: () => chin(1), long: () => chinLong(1), up: stopRepeat },
};
const pressed = {};

function held(name) {
  return Boolean(pressed[name]);
}
function keyEl(name) {
  return document.querySelector(`[data-key="${name}"]`);
}
function press(name) {
  const k = KEYS[name];
  if (!k || pressed[name]) return;
  const p = { long: false };
  pressed[name] = p;
  keyEl(name)?.classList.add("down");
  p.timer = setTimeout(() => {
    p.long = true;
    k.long();
  }, k.ms || LONG_MS);
}
function release(name) {
  const p = pressed[name];
  if (!p) return;
  delete pressed[name];
  clearTimeout(p.timer);
  keyEl(name)?.classList.remove("down");
  const k = KEYS[name];
  if (!p.long) k.click();
  k.up?.();
}

const shell = document.querySelector(".shell");
shell.addEventListener("pointerdown", (e) => {
  const el = e.target.closest("[data-key]");
  if (!el) return;
  e.preventDefault();
  el.setPointerCapture(e.pointerId);
  press(el.dataset.key);
});
for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
  shell.addEventListener(type, (e) => {
    const el = e.target.closest?.("[data-key]");
    if (el) release(el.dataset.key);
  });
}
window.addEventListener("blur", () => Object.keys(pressed).forEach(release));
