// 屏幕触控和电脑键盘。
panel.addEventListener("click", (e) => {
  if (state.off || state.booting || state.standby) return;
  const t = e.target;
  const goEl = t.closest("[data-go]");
  if (goEl) return go(goEl.dataset.go);

  // 正文默认点按分区：左边上一页，中间开关卡片，右边下一页（book_tap_zones.cc）。
  const body = t.closest(".read-body");
  if (body && !state.sheet) {
    const x = (e.clientX - body.getBoundingClientRect().left) / body.offsetWidth;
    if (x < 1 / 3) return void turnPage(-1);
    if (x > 2 / 3) return void turnPage(1);
    state.sheet = true;
    log("点屏幕中间：弹出阅读卡片。");
    return render();
  }

  const mode = t.closest("[data-mode]");
  if (mode) return switchMode(mode.dataset.mode);
  if (t.closest("[data-mute]")) return toggleMute();

  const provider = t.closest("[data-provider]");
  if (provider) {
    state.pendingProvider = provider.dataset.provider;
    if (state.pendingProvider !== state.provider) log(`云服务选了「${PROVIDERS[state.pendingProvider]}」，重启后才生效。`);
    return render();
  }
  if (t.closest("[data-reboot]")) return reboot();
  const link = t.closest("[data-link]");
  if (link) {
    state.link = link.dataset.link;
    log(`出口改成 ${state.link === "4G" ? "4G" : "Wi-Fi"}。官方云和自己的网关都走这条。`);
    return render();
  }
  if (t.closest("[data-usb]")) {
    state.usb = !state.usb;
    log(state.usb ? "USB 已连上，写在顶栏。" : "USB 拔掉了。");
    return render();
  }
});

panel.addEventListener("change", (e) => {
  const t = e.target;
  if (t.id === "mode") {
    state.mode = t.value;
    log(`以后进百问先进入${state.mode === "live" ? "通话" : "问答"}。不用重启。`);
  }
  if (t.id === "gateway") state.gateway = t.value;
  if (t.id === "pair") state.pair = t.value;
});

// 键盘：A = AI 唤醒，W / S = 加 / 减音量，P = 电源，M = 菜单，← → = 两颗灰键。
const KEYMAP = { a: "ai", w: "vol-up", s: "vol-down", p: "power", m: "menu", ArrowLeft: "prev", ArrowRight: "next" };
document.addEventListener("keydown", (e) => {
  if (e.target.closest("input, select")) return;
  const name = KEYMAP[e.key] || KEYMAP[e.key.toLowerCase()];
  if (!name || e.repeat) return;
  e.preventDefault();
  press(name);
});
document.addEventListener("keyup", (e) => {
  const name = KEYMAP[e.key] || KEYMAP[e.key.toLowerCase()];
  if (name) release(name);
});

log("假壳子就绪。云服务是「我的 AI」，进百问先进入问答。");
render();
