// 屏幕触控和电脑键盘。
panel.addEventListener("click", (e) => {
  if (state.off || state.booting || state.standby) return;
  const t = e.target;
  const diagnostic = t.closest("[data-diagnostic]");
  if (diagnostic) { state.diagnostic = diagnostic.dataset.diagnostic; return render(); }
  if (t.closest("[data-diagnostic-close]")) { state.diagnostic = ""; return render(); }
  if (t.closest("[data-wifi-test]")) { state.wifiTest = true; return render(); }
  if (t.closest("[data-wifi-test-close]")) { state.wifiTest = false; return render(); }
  if (t.closest("[data-vcom-query]")) { state.vcomRead = !state.vcomRead; return render(); }
  const black = t.closest("[data-black-depth]");
  if (black) { state.blackDepth = black.dataset.blackDepth; return render(); }
  if (t.closest("[data-device-restart]")) return reboot(false);
  if (t.closest("[data-device-off]")) return powerLong();
  const goEl = t.closest("[data-go]");
  if (goEl) return go(goEl.dataset.go);

  // 阅读卡片开着时，正文盖着网点；点网点收起卡片（和设备上点正文一样）。
  if (t.closest("[data-sheet-shade]")) {
    state.sheet = false;
    log("点卡片外面的网点：收起阅读卡片。");
    return render();
  }
  // 正文默认点按分区：左边上一页，中间开关卡片，右边下一页（book_tap_zones.cc）。
  const body = t.closest(".read-body");
  if (body && !state.sheet) {
    const box = body.getBoundingClientRect();
    const x = (e.clientX - box.left) / box.width;
    if (x < 1 / 3) return void turnPage(-1);
    if (x > 2 / 3) return void turnPage(1);
    if (state.float) return void v2CloseFloat("点书页中间");
    state.sheet = true;
    log("点屏幕中间：弹出阅读卡片。");
    return render();
  }

  const mode = t.closest("[data-mode]");
  if (mode) return switchMode(mode.dataset.mode);
  if (t.closest("[data-mute]")) return toggleMute();

  const sheet = t.closest("[data-sheet]");
  if (sheet) {
    state.sheetTab = Number(sheet.dataset.sheet);
    return render();
  }
  const stab = t.closest("[data-stab]");
  if (stab) {
    state.settingsTab = stab.dataset.stab;
    return render();
  }
  if (t.closest("[data-nobook]")) {
    log("这本在假壳子里还没有正文。");
    return;
  }
  const bookEl = t.closest("[data-book]");
  if (bookEl) {
    state.bookIndex = Number(bookEl.dataset.book);
    return go("book");
  }
  const shelf = t.closest("[data-shelf]");
  if (shelf) { state.shelfView = shelf.dataset.shelf; return render(); }
  const wallEl = t.closest("[data-wall]");
  if (wallEl && !wallEl.disabled) {
    state.wallIndex = Number(wallEl.dataset.wall);
    state.wallUse = WALLS[state.wallIndex][1];
    return go("wall");
  }
  const wuse = t.closest("[data-wuse]");
  if (wuse) {
    state.wallUse = wuse.dataset.wuse;
    WALLS.forEach((w) => { if (w[1] === state.wallUse) w[1] = ""; });
    WALLS[state.wallIndex][1] = state.wallUse;
    log(wuse.textContent);
    return render();
  }
  if (t.closest("[data-wclear]")) {
    WALLS[state.wallIndex][1] = "";
    state.wallUse = "";
    return render();
  }
  const ttab = t.closest("[data-ttab]");
  if (ttab) { state.taskTab = ttab.dataset.ttab; state.taskOpen = -1; return render(); }
  const task = t.closest("[data-task]");
  if (task) { state.taskOpen = Number(task.dataset.task); return render(); }
  const taskDo = t.closest("[data-task-do]");
  if (taskDo) {
    const item = state.todos[state.taskOpen];
    if (taskDo.dataset.taskDo === "done") item.done = true;
    else state.todos.splice(state.taskOpen, 1);
    state.taskOpen = -1;
    log(taskDo.dataset.taskDo === "done" ? "任务已完成" : "已删除");
    return render();
  }
  if (t.closest("[data-trefresh]")) { log("已刷新"); toast("已刷新"); return render(); }
  const ctab = t.closest("[data-ctab]");
  if (ctab) { state.cloudTab = ctab.dataset.ctab; return render(); }
  if (t.closest("[data-local-service]")) {
    if (!state.localService && state.link !== "wifi") return go("wifi");
    state.localService = !state.localService;
    state.localServiceMode = "wifi";
    state.localServiceUrl = "http://192.168.1.86";
    log(state.localService ? "本地传输已开启（模拟局域网地址）。" : "本地传输已关闭。");
    return render();
  }
  const localGo = t.closest("[data-local-go]");
  if (localGo) {
    state.localService = true;
    state.cloudTab = "local";
    state.localPage = localGo.dataset.localGo === "setup" ? "setup" : "home";
    log(localGo.dataset.localGo === "setup" ? "打开连接助理入口。" : "打开本地传输入口。");
    return render();
  }
  const setupBackend = t.closest("[data-setup-backend]");
  if (setupBackend) { state.setupBackend = setupBackend.dataset.setupBackend; return render(); }
  if (t.closest("[data-copy-prompt]")) { state.setupPromptCopied = true; toast("提示已复制"); return render(); }
  if (t.closest("[data-finish-pair]")) {
    const input = panel.querySelector("[data-pair-input]");
    state.setupPair = input?.value || "";
    if (!state.setupPair.includes("VALET_PAIR")) { toast("没有找到有效的 VALET_PAIR"); return; }
    state.paired = true;
    log(`${state.setupBackend} 配对完成。`);
    toast("设备已连接");
    return render();
  }
  const cfilter = t.closest("[data-cfilter]");
  if (cfilter) { state.cloudFilter = cfilter.dataset.cfilter; return render(); }
  const app = t.closest("[data-app]");
  if (app) { state.appName = app.dataset.app; return render(); }
  if (t.closest("[data-app-close]")) { state.appName = ""; return render(); }
  if (t.closest("[data-buddy-ok]")) {
    state.buddyBound = true;
    log("WorkBuddy 当作已授权。假的，没有真的扫码。");
    return render();
  }
  if (t.closest("[data-buddy-menu]")) { state.buddyMenu = !state.buddyMenu; return render(); }
  if (t.closest("[data-buddy-sync]")) {
    state.buddyMenu = false;
    log("消息已同步");
    toast("消息已同步");
    return render();
  }
  if (t.closest("[data-buddy-relink]")) {
    state.buddyMenu = false;
    state.buddyBound = false;
    log("重新绑定，回到扫码。");
    return render();
  }
  if (t.closest("[data-wscan]")) { log("扫描完成，共 3 个网络"); toast("扫描完成"); return render(); }
  if (t.closest("[data-wjoin]")) {
    if (isV2() && state.assistantSetup) {
      state.link = "wifi";
      state.localService = true;
      state.localServiceMode = "wifi";
      state.localServiceUrl = "http://192.168.1.86";
      back();
      return;
    }
    log("正在连接 " + t.closest("[data-wjoin]").dataset.wjoin);
    toast("WiFi连接中");
    return;
  }
  if (t.closest("[data-pscan]")) { log("正在扫描附近设备…"); toast("正在扫描"); return; }
  if (t.closest("[data-photo]")) {
    state.photo = true;
    log(state.lang === "en" ? "Photo ready. Paw becomes Send." : "拍好一张。猫爪换成发送。");
    return render();
  }
  if (t.closest("[data-photo-del]")) {
    state.photo = false;
    state.photoOpen = false;
    return render();
  }
  if (t.closest("[data-photo-open]")) { state.photoOpen = true; return render(); }
  if (t.closest("[data-photo-close]")) { state.photoOpen = false; return render(); }
  if (t.closest("[data-photo-send]")) {
    state.photo = false;
    state.answered = true;
    log(state.lang === "en" ? "Photo sent with the question." : "照片和这句话一起发出去了。");
    return render();
  }
  const bt = t.closest("[data-btmode]");
  if (bt) { state.btMode = bt.dataset.btmode; state.btDevices = state.btMode === "2"; return render(); }
  if (t.closest("[data-btdevices]")) { state.btDevices = !state.btDevices; return render(); }
  if (t.closest("[data-btscan]")) { toast("正在扫描设备"); return; }
  if (t.closest("[data-btreset]")) { state.btMode = "1"; toast("OK"); return render(); }
  const font = t.closest("[data-font]");
  if (font) { state.fontName = font.dataset.font; return render(); }
  const size = t.closest("[data-size]");
  if (size) {
    state.fontSize = Math.max(20, Math.min(50, state.fontSize + (size.dataset.size === "+" ? 5 : -5)));
    return render();
  }
  if (t.closest("[data-addmark]")) {
    if (!state.bookmarks.includes(state.page)) state.bookmarks.push(state.page);
    return render();
  }
  const mark = t.closest("[data-mark]");
  if (mark) { state.page = state.bookmarks[Number(mark.dataset.mark)]; state.sheet = false; return render(); }
  const toc = t.closest("[data-toc]");
  if (toc) { state.page = Number(toc.dataset.toc) === 0 ? 0 : Math.min(pages.length - 1, 1); state.sheet = false; return render(); }
  const setPick = (sel, key) => {
    const el = t.closest(sel);
    if (!el) return false;
    state[key] = el.getAttribute(sel.slice(1, -1));
    render();
    return true;
  };
  if (setPick("[data-margin]", "margin")) return;
  if (setPick("[data-spacing]", "spacing")) return;
  if (setPick("[data-para]", "para")) return;
  if (setPick("[data-turnstyle]", "turnStyle")) return;
  const shake = t.closest("[data-shake-turn]");
  if (shake) { state.shakeTurn = shake.dataset.shakeTurn === "on"; return render(); }
  const shakeLevel = t.closest("[data-shake-level]");
  if (shakeLevel) { state.shakeLevel = Number(shakeLevel.dataset.shakeLevel); return render(); }
  if (setPick("[data-writing]", "writing")) return;
  if (setPick("[data-standby]", "standbyStyle")) return;
  if (t.closest("[data-pconn]")) {
    state.pagerOn = !state.pagerOn;
    log(state.pagerOn ? "翻页器已连接" : "翻页器已断开");
    return render();
  }

  const provider = t.closest("[data-provider]");
  if (provider) {
    if (!providerChoices().some(([id]) => id === provider.dataset.provider)) return;
    state.pendingProvider = provider.dataset.provider;
    if (state.pendingProvider !== state.provider) log(`云服务选了「${PROVIDERS[state.pendingProvider]}」，重启后才生效。`);
    return render();
  }
  if (t.closest("[data-reboot]")) return reboot();
  if (t.closest("[data-net]")) {
    if (state.screen === "home" && state.link === "wifi") {
      log("首页、Wi-Fi 未当已连接：进入配网。");
      return go("wifi");
    }
    if (isV2()) {
      log("点了状态栏的网络图标，进入设置 → 网络。");
      return openSetting("net");
    }
    state.settingsTab = "net";
    log("点了状态栏的网络图标，进入设置 → 网络。");
    return go("settings");
  }
  const link = t.closest("[data-link]");
  if (link) {
    if (isV2()) {
      // 新设计：自动 / 4G / Wi-Fi；切换不重启（settleLink 在 design.js）
      if (link.dataset.link === state.linkMode) return;
      state.linkMode = link.dataset.link;
      settleLink(L(`上网方式改成「${state.linkMode === "auto" ? "自动" : state.linkMode === "4G" ? "4G" : "Wi-Fi"}」`, "Connection changed"));
      return render();
    }
    if (link.dataset.link === state.link) return;
    state.link = link.dataset.link;
    log(`上网方式改成${state.link === "4G" ? " 4G，图标换成信号格" : " Wi-Fi，图标换成扇形"}。真机会重启。`);
    return render();
  }
  const set = (sel, key, msg) => {
    const el = t.closest(sel);
    if (!el) return false;
    state[key] = el.getAttribute(sel.slice(1, -1));
    if (msg) log(msg);
    render();
    return true;
  };
  if (set("[data-sim]", "sim")) return;
  if (set("[data-theme]", "theme", isV2() ? "应用页样式改了。首页第二屏能看到。" : "首页主题改了。返回首页就能看到。")) return;
  if (set("[data-lang]", "lang")) return;
  if (set("[data-haptic]", "haptic")) return;
  if (set("[data-hlevel]", "hapticLevel")) return;
  if (set("[data-refresh]", "refresh")) return;
  if (set("[data-light]", "light")) return;
  const charge = t.closest("[data-charge]");
  if (charge) { state.chargeMa = Number(charge.dataset.charge); return render(); }
  const mhz = t.closest("[data-mhz]");
  if (mhz) { state.idleMhz = Number(mhz.dataset.mhz); return render(); }
  const grace = t.closest("[data-grace]");
  if (grace) { state.netGrace = Number(grace.dataset.grace); return render(); }
  const tts = t.closest("[data-tts]");
  if (tts) { state.tts = tts.dataset.tts === "on"; return render(); }
  const logBtn = t.closest("[data-log]");
  if (logBtn) { state.logOn = logBtn.dataset.log === "on"; return render(); }
  const def = t.closest("[data-default]");
  if (def) {
    state.mode = def.dataset.default;
    log(`以后进百问先进入${state.mode === "live" ? "通话" : "问答"}。不用重启。`);
    return render();
  }
  if (t.closest("[data-usb]")) {
    state.usb = !state.usb;
    log(state.usb ? "模拟 U 盘已启用。状态栏不显示 USB，存储页会改文案。" : "模拟 U 盘已停用。");
    return render();
  }
  const bright = t.closest("[data-bright]");
  if (bright) {
    state.brightness = Math.max(0, Math.min(100, state.brightness + (bright.dataset.bright === "+" ? 10 : -10)));
    return render();
  }
});

panel.addEventListener("change", (e) => {
  const t = e.target;
  if (t.id === "gateway") state.gateway = t.value;
  if (t.id === "pair") state.pair = t.value;
});

function scrollablePane(target) {
  return target.closest?.(".v-pane, .set .pane") || null;
}
let drag = null;
let scrollDot = null;
function moveScrollDot(x, y) {
  if (!scrollDot) {
    scrollDot = document.createElement("i");
    scrollDot.className = "scroll-dot";
    document.body.appendChild(scrollDot);
  }
  scrollDot.style.left = `${x}px`;
  scrollDot.style.top = `${y}px`;
}
function endScrollDrag() {
  if (!drag) return;
  drag.el.classList.remove("dragging");
  drag = null;
  scrollDot?.remove();
  scrollDot = null;
}
function beginScrollDrag(e) {
  if (blocked() || e.button || drag) return;
  if (e.target.closest("input, select, [data-level], [data-key]")) return;
  const el = scrollablePane(e.target) || scrollablePane(e);
  if (!el) return;
  drag = { el, y: e.clientY, top: el.scrollTop, moved: false, id: e.pointerId };
}
panel.addEventListener("pointerdown", beginScrollDrag);
window.addEventListener("pointermove", (e) => {
  if (!drag) return;
  const dy = e.clientY - drag.y;
  if (!drag.moved && Math.abs(dy) < 4) return;
  const el = drag.el;
  const top = drag.top;
  if (!drag.moved) {
    drag.moved = true;
    el.classList.add("dragging");
  }
  el.scrollTop = top - dy;
  moveScrollDot(e.clientX, e.clientY);
  e.preventDefault();
});
panel.addEventListener("click", (e) => {
  if (!drag?.moved) return;
  e.preventDefault();
  e.stopPropagation();
}, true);
document.addEventListener("pointerup", endScrollDrag);
document.addEventListener("pointercancel", endScrollDrag);

// 屏上的「按住说话」：按下开始听，松手发送。画面会重画，所以松手在 document 上等。
let screenHold = false;
panel.addEventListener("pointerdown", (e) => {
  if (blocked() || (state.screen !== "ai" && !state.float) || state.live || !e.target.closest("[data-hold]")) return;
  e.preventDefault();
  screenHold = true;
  log("按住屏上的「按住说话」：在听。");
  startListening();
});
document.addEventListener("pointerup", () => {
  if (!screenHold) return;
  screenHold = false;
  stopListeningAndSend();
});
function dropHold() {
  if (!screenHold) return;
  screenHold = false;
  state.listening = false;
  render();
}
document.addEventListener("pointercancel", dropHold);
window.addEventListener("blur", dropHold);

// 键盘：A = AI 唤醒，W / S = 加 / 减音量，P = 电源，M = 菜单，← → = 两颗灰键。
const KEYMAP = { a: "ai", w: "vol-up", s: "vol-down", p: "power", m: "menu", ArrowLeft: "prev", ArrowRight: "next" };
// 在配置页的输入框里打字，不能当成设备按键。
const typing = (e) => Boolean(e.target.closest?.("input, select, textarea, [contenteditable]"));
document.addEventListener("keydown", (e) => {
  if (typing(e)) return;
  const name = KEYMAP[e.key] || KEYMAP[e.key.toLowerCase()];
  if (!name || e.repeat) return;
  e.preventDefault();
  press(name);
});
document.addEventListener("keyup", (e) => {
  if (typing(e)) return;
  const name = KEYMAP[e.key] || KEYMAP[e.key.toLowerCase()];
  if (name) release(name);
});

log("假壳子就绪。页头可以在原始设计和新设计之间切。");
render();

// Regional firmware selection is separate from Chinese/English UI language.
document.querySelector("#edition").addEventListener("change", (event) => {
  state.edition = event.target.value;
  const available = providerChoices().map(([id]) => id);
  if (!available.includes(state.provider)) state.provider = available[0];
  if (!available.includes(state.pendingProvider)) state.pendingProvider = state.provider;
  stopSession(); render();
});
