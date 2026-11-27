// 把按键、屏幕和页面右侧的说明接起来。
const nowEl = document.querySelector("#now");
const aboutEl = document.querySelector("#about");
const jumpEl = document.querySelector("#jump");

function keyHelp() {
  const v = isV2();
  const askName = v ? "提问" : "百问";
  if (state.off) return [["电源", "短按开机"]];
  if (state.booting) return [["", "开机中"]];
  if (state.standby) return [["AI 唤醒", `长按：醒来直接进${askName}`], ["电源", "短按醒来；长按 3 秒关机"], ["其他键", "不响应"]];
  const s = state.screen;
  const live = s === "ai" && state.provider === "agent" && state.live;
  let ai = v ? "长按：从这一屏直接提问" : "不响应";
  if (s === "home" || s === "detail") ai = `长按进${askName}，不松手直接开始听`;
  if (s === "reader") ai = state.float
    ? (state.speaking ? "短按打断" : "短按收起悬窗") + (state.live ? "" : "；长按在悬窗里说话")
    : v ? `短按${state.sheet ? "收起" : "打开"}工具面板；长按在页面下方的悬窗里问这一页`
      : `短按${state.sheet ? "收起" : "弹出"}阅读卡片；长按带这一页进${askName}`;
  if (s === "ai") ai = live ? "短按打断助手说话" : "按住说话，松开发送；短按停下回答";
  const vol = readingActive() ? "加 = 上一页，减 = 下一页，长按连翻" : "调音量，长按到最大或静音";
  let menu = "返回上一级；长按回首页";
  if (s === "home") menu = state.homePage ? "回到首页第一屏" : "已在首页";
  if (s === "reader") menu = state.float ? "收起悬窗" : state.sheet ? (v && state.sheetTab !== "home" ? "回到工具首页" : "收起面板") : "离开正文；长按回首页";
  if (s === "ai" && state.aiBack === "reader") menu = `回第 ${state.page + 1} 页${state.floatExpanded ? "的悬窗" : ""}；长按回首页`;
  let gray = "左键返回";
  if (s === "home") gray = v ? "在「今天」和「全部应用」之间翻" : "翻首页的两屏应用";
  if (s === "reader") gray = state.sheet ? (v ? "左键收起面板" : "左键收起卡片") : (state.float && state.floatExpanded) ? "翻回答；第一页再往前就收起悬窗" : state.float ? "照样翻书，悬窗跟着到那一页" : "翻页，长按连翻";
  if (s === "ai") gray = live ? "左键返回" : "翻回答；第一页再往前就返回";
  if (s === "settings") gray = "内容超出一屏时上下翻；到顶后再按左键返回";
  return [["AI 唤醒", ai], ["加 / 减音量", vol], ["电源", "短按待机；长按 3 秒关机"], ["菜单（橙点）", menu], ["两颗灰键", gray]];
}

function screenHtml() {
  if (state.off) return `<img class="full" src="${SIM_IMAGES["bg_shutdown_zh.png"]}" alt="关机画面：按键说明" />`;
  if (state.booting) return `<div class="boot">Metalio</div>`;
  if (state.standby) return isV2() ? v2Standby() : standbyScreen();
  if (isV2()) return v2Screen() + (state.quick ? v2QuickPanel() : "");
  return { home, detail, reader, ai, settings, wallpaper, wall, tasks, transfer, store, shelf, book, buddy, wifi, pager }[state.screen]();
}

function render() {
  let html = screenHtml();
  if (state.toast && !state.off && !state.booting) html += `<div class="toast">${esc(state.toast)}</div>`;
  panel.classList.toggle("v2", isV2());
  panel.innerHTML = html;
  if (isV2() && state.float) v2SnapFloatSoon();
  if (isV2() && v2.sel && !v2.drag) v2PlaceSelPop();
  const where = state.off ? "关机" : state.booting ? "开机中" : state.standby ? "待机" : names()[state.screen];
  nowEl.innerHTML = `<p>现在在「${where}」，按键是：</p><dl>${keyHelp()
    .map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("")}</dl>`;
}

// 页头的页签：同一台假机器，两套界面。状态（读到哪一页、设置）是共用的。
const ABOUT = {
  v2: ["新设计 · Valet 随行", "斜切、黑白、整页翻，这些官方的画法没换。换的是助手在哪一层，以及设置怎么找。", [
    "首页永远是两块斜切。连上自己的助理是「提问」和「通话」，下面接着读和待办；没连上时没有通话，右边那块换成「接着读」，下面是今天的待办列表。别的应用在第二屏。",
    "「这一页 / 随口问」决定谈什么，提问和通话只是怎么说。每一页的问答各自留着。",
    "正文像一页纸书：页眉只写章节，页码在页脚正中。点屏幕中间打开工具面板（问这一页、目录、书签、排版），一次只露一项。问过的页右上角有「注」。",
    "问这一页不用切出去：问答在页面下方的悬窗里，书页不动，侧键照样翻书，悬窗跟着到那一页。回答长时点右上角展开图标原地拉满全屏看全部内容，再点缩回半屏，或点 ✕ 直接关掉回到当前页；左侧喇叭图标可切换回答朗读/静音并持续保持。",
    "设置按要做的事分六项，每一项直接写着现在的值。前光和音量在最上面。",
    "第 2 页的「蟠」当作机内字库没有的字（假数据），画成方框，页脚会提示。设置 → 阅读与字库里装上扩展字库就能显示。",
  ]],
  classic: [`原始设计 · 官方 ${UPSTREAM.prototypeVersion} 原型`, "源码基线已同步到 2.0.82；2.0.81 的界面变化已结合公开源码复核。保留自建扩展。", [
    "首页是斜切时钟和六格应用，灰键翻到第二屏。",
    "百问按住说话，回答分页。云服务选「自建」时，页底可以切到通话。",
    "正文里短按 AI 唤醒键出阅读卡片，长按才进百问。官方百问拿不到正在读的那一页。",
    "设置是左侧十四个页签，云服务在「对话」里，换了要重启。",
  ]],
};
const JUMPS = [["home", "首页"], ["reader", "正文"], ["ask", "提问"], ["call", "通话"], ["settings", "设置"], ["standby", "待机"], ["setup", "配置页"]];

function renderAbout() {
  document.querySelector("#upstream-status").textContent = `公开源码 ${UPSTREAM.sourceVersion} · 官方固件 ${UPSTREAM.releaseVersion} · 原型 ${UPSTREAM.prototypeVersion}（界面已按源码复核，底层硬件行为仍需实机验证）`;
  const [title, lede, points] = ABOUT[state.design];
  aboutEl.innerHTML = `<h2>${title}</h2><p class="lede">${lede}</p><ul class="points">${points.map((p) => `<li>${p}</li>`).join("")}</ul>`;
  jumpEl.innerHTML = JUMPS.filter(([id]) => id !== "setup" || isV2()).map(([id, name]) => `<button data-jump="${id}">${id === "ask" && !isV2() ? "百问" : name}</button>`).join("");
  document.querySelectorAll("[data-design]").forEach((b) => {
    const on = b.dataset.design === state.design;
    b.classList.toggle("on", on);
    b.setAttribute("aria-selected", on);
  });
}

function setDesign(design) {
  if (design === state.design) return;
  if (state.screen === "ai") {
    if (isV2()) v2LeaveAi();
    stopSession();
  }
  state.design = design;
  state.homePage = 0;
  state.settingsScreen = "list";
  v2.direct = null;
  if (state.screen === "ai") enterAi(state.aiBack);
  log(`切到${isV2() ? "新设计" : "原始设计"}。读到第 ${state.page + 1} 页和各项设置都没动。`);
  renderAbout();
  flash();
  render();
}

function jumpTo(id) {
  state.off = false;
  state.booting = false;
  state.standby = false;
  cancel("boot");
  goHome();
  if (id === "standby") {
    state.standby = true;
    return render();
  }
  if (id === "reader") {
    if (isV2()) return go("reader");
    go("detail");
    return go("reader");
  }
  if (id === "ask") return go("ai");
  if (id === "call") {
    if (isV2()) return v2GoLive();
    go("ai");
    return switchMode("live");
  }
  if (id === "settings") go("settings");
  // 配置页：设备停在「添加助理」，在 Wi-Fi 上就走同一个 Wi-Fi，否则开临时热点；浏览器小窗直接打开
  if (id === "setup" && isV2()) {
    openSetting("talk");
    SetupPage.reset();
    const wifi = state.link === "wifi";
    Object.assign(state, { assistantDetail: null, assistantSetup: true, assistantAdvanced: false, assistantRename: false,
      localService: true, localServiceMode: wifi ? "wifi" : "ap", localServiceUrl: wifi ? "http://192.168.1.86" : "http://192.168.4.1" });
    log(`直接打开配置页（设备在「添加助理」，${wifi ? "同一个 Wi-Fi" : "临时热点"}）。`);
    SetupPage.open();
    return render();
  }
}

document.querySelector(".design-tabs").addEventListener("click", (e) => {
  const b = e.target.closest("[data-design]");
  if (b) setDesign(b.dataset.design);
});
jumpEl.addEventListener("click", (e) => {
  const b = e.target.closest("[data-jump]");
  if (b) jumpTo(b.dataset.jump);
});
document.querySelector("[data-reset]").addEventListener("click", () => {
  sessionStorage.setItem("design", state.design);
  location.reload();
});
if (sessionStorage.getItem("design") === "classic") state.design = "classic";
sessionStorage.removeItem("design");
renderAbout();

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
function release(name, cancelled = false) {
  const p = pressed[name];
  if (!p) return;
  delete pressed[name];
  clearTimeout(p.timer);
  keyEl(name)?.classList.remove("down");
  const k = KEYS[name];
  if (!p.long && !cancelled) k.click();
  k.up?.();
}

const shell = document.querySelector(".shell");
let keyPointer = null;
let lastKeyRelease = null;
function finishKeyInput(e, cancelled = false) {
  if (!keyPointer) return;
  const { name } = keyPointer;
  keyPointer = null;
  // 只有真正执行过动作才去重。取消 / 丢失捕获后，浏览器的 click 仍可兜底。
  if (!cancelled || pressed[name]?.long) lastKeyRelease = { name, at: e.timeStamp };
  release(name, cancelled);
}

// 手机用原生 Touch Events；不依赖浏览器 / 内嵌预览的 pointer capture。
shell.addEventListener("touchstart", (e) => {
  const el = e.target.closest("[data-key]");
  if (!el || keyPointer || e.touches.length !== 1) return;
  e.preventDefault();
  lastKeyRelease = null;
  keyPointer = { id: e.changedTouches[0].identifier, name: el.dataset.key, touch: true };
  press(el.dataset.key);
}, { passive: false });
for (const type of ["touchend", "touchcancel"]) {
  document.addEventListener(type, (e) => {
    if (!keyPointer?.touch || !Array.from(e.changedTouches).some((t) => t.identifier === keyPointer.id)) return;
    if (e.cancelable) e.preventDefault();
    finishKeyInput(e, type === "touchcancel");
  }, { passive: false });
}
shell.addEventListener("pointerdown", (e) => {
  const el = e.target.closest("[data-key]");
  if (!el || (e.pointerType === "mouse" && e.button !== 0) || keyPointer) return;
  if (e.pointerType === "touch" && "TouchEvent" in window) return;
  e.preventDefault();
  lastKeyRelease = null;
  keyPointer = { id: e.pointerId, name: el.dataset.key };
  press(el.dataset.key);
  // 文档级 pointerup 也会收尾，捕获不可用时仍可正常点按。
  try { el.setPointerCapture?.(e.pointerId); } catch {}
});
for (const type of ["pointerup", "pointercancel"]) {
  document.addEventListener(type, (e) => {
    if (!keyPointer || keyPointer.touch || keyPointer.id !== e.pointerId) return;
    finishKeyInput(e, type === "pointercancel");
  });
}
// 键盘 / 辅助功能生成的 click 也能操作；触摸松手后的合成 click 不能再翻一次。
shell.addEventListener("click", (e) => {
  const el = e.target.closest("[data-key]");
  if (!el) return;
  e.preventDefault();
  const name = el.dataset.key;
  if (e.detail !== 0 && lastKeyRelease?.name === name && e.timeStamp - lastKeyRelease.at < 1000) return;
  if (keyPointer?.name === name) finishKeyInput(e);
  else KEYS[name]?.click();
});
window.addEventListener("blur", () => {
  keyPointer = null;
  Object.keys(pressed).forEach((name) => release(name, true));
});
