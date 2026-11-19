// 百问页和设置页。
function stateWord() {
  if (state.provider === "agent" && state.live) return `通话 · ${state.liveMuted ? "静音" : state.speaking ? "说" : "听"}`;
  if (state.listening) return "聆听中...";
  if (state.speaking) return "回答中...";
  return "待命";
}

function chip() {
  if (state.fromPage === null) return "";
  if (state.provider === "agent") {
    return `<div class="chip">带上第 ${state.fromPage + 1} 页：${esc(pages[state.fromPage][0].slice(0, 14))}…</div>`;
  }
  return `<div class="chip dim">官方百问拿不到正在读的这一页</div>`;
}

const FEATURES = [
  ["记录待办", "帮我把重要的事项整理成待办"],
  ["旅游助手", "攻略、美食、天气、交通，一键规划"],
  ["探索世界", "解答各种疑问，探索未知"],
  ["陪伴聊天", "放松交流、分享心情、打发时间"],
];

function welcome() {
  const name = state.provider === "agent" ? "自建" : PROVIDERS[state.provider] + "AI";
  const rows = FEATURES
    .filter((_, i) => state.provider === "baiwen" || state.provider === "agent" || i > 0)
    .map(([title, detail]) => `<div class="feat"><i></i><span><b>${title}</b><em>${detail}</em></span></div>`).join("");
  return `<div class="welcome">
    <h2>${name}</h2>
    <p class="hello">你好，<br>我是你的智能助手</p>
    <p class="ask">有什么可以帮你？</p>
    <div class="talk">长<br>按<br>对<br>话</div>
    <div class="feats">${rows}</div>
  </div>`;
}

function askBody() {
  if (state.listening) return `<div class="big">${state.lang === "en" ? "Listening..." : "聆听中..."}</div>`;
  if (state.answered) {
    const list = answerFor();
    const shot = state.photo ? `<p class="shot">▣</p>` : "";
    return `<div class="qa">${shot}<p><b>${t("user")}</b>${esc(questionFor())}</p><p><b>${t("aiRole")}</b>${glyphs(esc(list[state.answerPage]))}</p></div>`;
  }
  return welcome();
}

function liveBody() {
  const fallback = state.liveMuted ? "麦克风关着。点「取消静音」再说。" : "正在听，直接说。";
  const on = state.speaking || (state.listening && !state.liveMuted);
  return `<div class="live"><div class="wave${on ? " on" : ""}">${"<i></i>".repeat(9)}</div>
    <p class="caption">${esc(state.caption || fallback)}</p>
    <p class="note">字幕整页换，不滚动。AI 在说时短按 AI 唤醒打断。</p></div>`;
}

function pageCount() {
  return state.answered && !state.live ? `${state.answerPage + 1} / ${answerFor().length}` : "";
}

function aiFoot() {
  const pages = state.answered && !state.live ? `<div class="pager">${pageCount()}</div>` : "";
  const viewer = state.photoOpen ? `<div class="mask"><div class="dialog"><div class="shot big"></div><button data-photo-close>${t("close")}</button></div></div>` : "";
  if (state.provider !== "agent" || !state.live) {
    const right = state.photo
      ? `<button class="send" data-photo-send>${state.lang === "en" ? "Send" : "发送"}</button>`
      : `<i class="paw"></i>`;
    const thumb = state.photo ? `<button class="thumb" data-photo-open>▣<i data-photo-del>×</i></button>` : "";
    const cam = state.provider === "baiwen" || state.provider === "agent" ? `<button class="cam" data-photo title="拍照"></button>` : `<i></i>`;
    return `${viewer}${thumb}${pages}<div class="holdbar">${cam}<span data-hold>${t("holdTalk")}</span>${right}</div>`;
  }
  const right = state.live
    ? `<button data-mute>${state.liveMuted ? "取消静音" : "静音"}</button>`
    : `<span>${pageCount()}</span>`;
  return `${pages}<div class="foot"><span class="seg">
      <button data-mode="ask" class="${state.live ? "" : "on"}">问答</button><button data-mode="live" class="${state.live ? "on" : ""}">通话</button>
    </span>${right}</div>`;
}

function ai() {
  const body = state.provider === "agent" && state.live ? liveBody() : askBody();
  return `${bar(stateWord())}<div class="ai">${chip()}<div class="ai-main">${body}</div>${aiFoot()}</div>`;
}

function settingsTabs() {
  return [
    ["net", t("tabNet")], ["theme", t("tabTheme")], ["lang", t("tabLang")], ["haptic", t("tabHaptic")],
    ["power", t("tabPower")], ["charge", t("tabCharge")], ["screen", t("tabScreen")],
    ["storage", t("tabStorage")], ["talk", t("tabTalk")], ["bt", t("tabBt")],
    ["pager", t("tabPager")], ["test", t("tabTest")], ["log", t("tabLog")], ["about", t("tabAbout")],
  ];
}

function opts(items, current, attr) {
  return `<div class="choices">${items.map(([id, name]) =>
    `<button class="choice${current === id ? " on" : ""}" ${attr}="${id}">${name}</button>`).join("")}</div>`;
}
function cur(text) {
  return `<p class="cur">${t("now")}: ${text}</p>`;
}

function settingsPane() {
  const id = state.settingsTab;
  if (id === "net") {
    const mode = state.link === "4G" ? "4G" : "WiFi";
    const sim = state.link === "4G"
      ? `<h3>SIM 卡</h3><p class="note">切换后设备将自动重启</p>${opts([["int", "内置卡"], ["ext", "外置卡"]], state.sim, "data-sim")}${cur(state.sim === "int" ? "内置卡" : "外置卡")}`
      : "";
    return `<p class="cur">上网方式：${mode}</p>
      ${opts([["4G", "4G 上网"], ["wifi", "WiFi 上网"]], state.link, "data-link")}
      <p class="note">切换后设备将自动重启</p>
      <h3>WiFi 状态：</h3>${cur(state.link === "wifi" ? "启用中" : "未使用")}
      <button class="choice" data-go="wifi">进入配网</button>
      <p class="note">进入后按提示连接设备热点</p>${sim}`;
  }
  if (id === "theme") {
    const themes = [["white", t("thWhite")], ["gray", t("thGray")], ["border", t("thBorder")], ["slash", t("thSlash")], ["pixel", t("thPixel")]];
    return `<h3>${t("themeTitle")}</h3><p class="note">${t("themeHint")}</p>
      ${opts(themes, state.theme, "data-theme")}
      ${cur(themes.find(([id]) => id === state.theme)[1])}`;
  }
  if (id === "lang") {
    return `<h3>${t("langTitle")}</h3><p class="note">${t("langHint")}</p>
      ${opts([["zh", t("zh")], ["en", "English"]], state.lang, "data-lang")}
      ${cur(state.lang === "zh" ? t("zh") : "English")}`;
  }
  if (id === "haptic") {
    return `<h3>按键震动</h3><p class="note">界面按钮短震反馈</p>
      ${opts([["on", "开启"], ["off", "关闭"]], state.haptic, "data-haptic")}
      <h3>震动强度</h3>
      ${opts([["low", "弱"], ["mid", "中"], ["high", "强"]], state.hapticLevel, "data-hlevel")}`;
  }
  if (id === "power") {
    return `<h3>空闲降频（默认320Mhz）</h3><p class="note">无操作空闲时的 CPU 频率</p>
      ${opts([["160", "160"], ["240", "240"], ["320", "320"]], String(state.idleMhz), "data-mhz")}
      <h3>保网时长（默认 30 秒）</h3><p class="note">无保网需求后仍联网的时长；待机仍断网</p>
      ${opts([["30", "30 秒"], ["60", "60 秒"], ["120", "120 秒"], ["0", "永不"]], String(state.netGrace), "data-grace")}
      <h3>浅睡待机（默认 5 分钟）</h3><p class="note">无操作 5 分钟后进入浅睡待机</p>
      <h3>自动关机（默认 30 分钟）</h3><p class="note">待机累计 30 分钟后自动关机</p>`;
  }
  if (id === "charge") {
    return `<h3>充电电流</h3><p class="note">默认 1A，选择后自动保存</p>
      ${opts([["500", "0.5A"], ["1000", "1A"], ["1500", "1.5A"]], String(state.chargeMa), "data-charge")}`;
  }
  if (id === "screen") {
    const styles = [["classic", state.lang === "en" ? "Classic standby" : "经典待机"], ["wallpaper", state.lang === "en" ? "Wallpaper standby" : "壁纸待机"]];
    return `<h3>刷新模式</h3>
      ${opts([["clear", "清晰"], ["balanced", "均衡"], ["fast", "快速"]], state.refresh, "data-refresh")}
      <p class="note">${state.refresh === "clear" ? "细节更清晰，残影更少。" : state.refresh === "balanced" ? "兼顾画质与速度，可能加重残影。" : "保留灰阶，响应更快；可能加重残影。"}</p>
      <h3>${state.lang === "en" ? "Standby" : "待机样式"}</h3>
      ${opts(styles, state.standbyStyle, "data-standby")}
      <h3>前光</h3><p class="note">色温与亮度 0–100</p>
      ${opts([["cool", "冷光"], ["warm", "暖光"], ["both", "冷暖光"], ["off", "关闭"]], state.light, "data-light")}
      <div class="opt"><span>亮度：${state.brightness}</span><span><button data-bright="-">-</button> <button data-bright="+">+</button></span></div>
      ${calibrationControls()}`;
  }
  if (id === "storage") {
    return `<h3>内存卡</h3><p class="cur">${state.usb ? "内存卡：电脑占用中" : "内存卡：已插入"}</p>
      <p class="cur">${state.usb ? "容量：导出中，停用后可查看" : "容量：12.4GB/29.1GB"}</p>
      <button class="choice" data-usb>${state.usb ? "停用模拟 U 盘" : "启用模拟 U 盘"}</button>
      <p class="note">${state.usb ? "已启用，请在电脑访问。停用前请先弹出。" : "启用后电脑可以将本机识别为 U 盘。"}</p>`;
  }
  if (id === "talk") {
    const choices = providerChoices().map(([pid, name]) =>
      `<button class="choice${state.pendingProvider === pid ? " on" : ""}" data-provider="${pid}">${name}</button>`).join("");
    const mine = state.pendingProvider === "agent" ? `
      <label>网关地址<input id="gateway" value="${esc(state.gateway)}" /></label>
      <label>配对码<input id="pair" value="${esc(state.pair)}" /></label>
      <p class="note">进百问先进入</p>
      ${opts([["ask", "问答"], ["live", "通话"]], state.mode, "data-default")}
      <p class="note">改问答或通话不用重启。</p>` : `<p class="note">切换后设备将自动重启。官方三项都是按住说话。</p>`;
    const reboot = state.pendingProvider !== state.provider ? `<button class="wide" data-reboot>保存并重启</button>` : "";
    return `<h3>云服务切换</h3><div class="choices">${choices}</div>${mine}
      <h3>语音播报</h3><p class="note">控制百问AI是否播报语音。关闭后仅显示文字。</p>
      ${opts([["on", "开启 TTS"], ["off", "关闭 TTS"]], state.tts ? "on" : "off", "data-tts")}
      ${cur(state.tts ? "开启 TTS" : "关闭 TTS")}${reboot}`;
  }
  if (id === "bt") {
    const modes = [["1", t("localAudio")], ["2", t("btSend")], ["3", t("btRecv")], ["5", t("btKeys")]];
    const desc = { 1: t("mode1"), 2: t("mode2"), 3: t("mode3"), 5: t("mode5") }[state.btMode];
    const devices = state.btDevices ? `<div class="frame"><p class="empty">${t("noDevice")}</p><button data-btscan>${t("scanDev")}</button></div>` : "";
    return `<h3>${state.lang === "en" ? "Mode" : "模式切换"}</h3>
      ${opts(modes, state.btMode, "data-btmode")}
      <p class="note">${desc}</p>
      <h3>${state.lang === "en" ? "Bluetooth name" : "蓝牙名称"}</h3>
      <p>${esc(state.btName)}</p>
      <button class="choice" data-btdevices>${t("btDevices")}</button>
      ${devices}
      <h3>${state.lang === "en" ? "Reset" : "蓝牙复位"}</h3>
      <button class="choice" data-btreset>${state.lang === "en" ? "Reset" : "复位"}</button>`;
  }
  if (id === "pager") {
    return `<h3>翻页器</h3><button class="choice" data-go="pager">设备管理</button><button class="choice">按键自定义</button>
      <p class="note">尚无已保存设备，点击附近翻页器连接后将自动保存。默认 A 上一页、B 下一页。</p>`;
  }
  if (id === "test") {
    return diagnosticPane();
  }
  if (id === "log") {
    return `<h3>设备日志</h3><p class="note">开启后将运行日志写入 SD 卡：metalio/e-ink/logs/metalio_logs.txt</p>
      ${opts([["on", "开启"], ["off", "关闭"]], state.logOn ? "on" : "off", "data-log")}
      <button class="choice">上传日志</button><button class="choice">清除日志</button>`;
  }
  return `<button class="about"><b>设备型号</b><span>Metalio E-Ink4 Plus</span></button>
    <button class="about"><b>芯片型号</b><span>ESP32-S31</span></button>
    <button class="about"><b>CPU 核心</b><span>2 核</span></button>
    <button class="about"><b>点击检测更新</b><span>${prototypeVersionLabel()}</span></button>
    <button class="about"><b>MAC 地址</b><span>00:00:00:00:00:00</span></button>
    <button class="about"><b>Flash 容量</b><span>16 MB</span></button>
    <button class="about"><b>PSRAM 总大小</b><span>Octal</span></button>`;
}

function settings() {
  const tabs = settingsTabs().map(([id, name]) =>
    `<button data-stab="${id}" class="${state.settingsTab === id ? "on" : ""}">${name}</button>`).join("");
  return `${bar()}<div class="settings"><div class="stabs">${tabs}</div><div class="spane">${settingsPane()}</div></div>`;
}
