// 每个界面一个函数，返回面板里的 HTML。真屏 684×1216，这里缩到 246×438。
// 文案和结构对齐固件 2.0.80，视觉按重新设计的风格画。
function names() {
  return {
    home: t("home"), detail: t("detail"), reader: t("reader"), ai: t("ai"),
    settings: t("settings"), wallpaper: t("wallpaper"), tasks: t("tasks"),
    transfer: t("transfer"), store: t("store"), shelf: t("shelf"), book: t("book"),
    wall: t("wall"), buddy: "workbuddy", wifi: t("wifi"), pager: t("pager"),
    ...(isV2() ? { ai: L("助手", "Assistant"), detail: L("阅读", "Reading") } : {}),
  };
}

function esc(text) {
  return String(text).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}

// 状态栏
function netIcon() {
  if (state.link === "4G") return `<i class="sig" data-net><b></b><b></b><b></b><b></b></i>`;
  return `<i class="wifi" data-net></i>`;
}
function battIcon() {
  const n = state.charging ? 4 : state.battery <= 10 ? 0 : state.battery <= 30 ? 1 : state.battery <= 60 ? 2 : state.battery <= 85 ? 3 : 4;
  return `<i class="batt${state.charging ? " bolt" : ""}" data-fill="${n}"></i>`;
}
function bar(center = "10:48") {
  const mute = state.volume === 0 ? `<i class="mute"></i>` : "";
  return `<div class="bar">${netIcon()}<span class="c">${center}</span><span class="right">${mute}<span class="pct">${state.battery}%</span>${battIcon()}</span></div>`;
}

// 斜切首页应用格
function tiles() {
  return [
    ["detail", t("books"), "book"],
    ["wallpaper", t("walls"), "wallpaper"],
    ["ai", t("ask"), "teacher"],
    ["tasks", t("todos"), "task"],
    ["transfer", t("cloud"), "cloud"],
    ["settings", t("gear"), "setting"],
    ["buddy", "workbuddy", "task"],
    ["store", t("shop"), "cloud"],
  ];
}

function tilePoints(row, side) {
  const y0 = row * 70;
  const y1 = y0 + 64;
  const [top, bottom] = row === 1 ? [96, 140] : [140, 96];
  if (side === 0) return `12,${y0} ${top},${y0} ${bottom},${y1} 12,${y1}`;
  return `${top + 7},${y0} 234,${y0} 234,${y1} ${bottom + 7},${y1}`;
}

// 首页：AI 区域拆成「问答」和「通话」两块，分开进
function home() {
  if (state.theme === "pixel") return pixelHome();
  if (state.theme !== "slash") return classicHome();
  const start = state.homePage * 6;
  const pageTiles = tiles().slice(start, start + 6);
  const drawn = pageTiles.map(([go, name, icon], i) => {
    const row = Math.floor(i / 2);
    const side = i % 2;
    const dark = (row + side) % 2 === 0;
    const y = row * 70 + 32;
    const tight = side === 0 && name.length > 4;
    const iconX = side === 0 ? (tight ? 16 : 22) : 196;
    const text = side === 0
      ? `<text x="${iconX + 36}" y="${y + 4.5}" clip-path="url(#clip-${i})">${name}</text>`
      : `<text x="${iconX - 6}" y="${y + 4.5}" text-anchor="end" clip-path="url(#clip-${i})">${name}</text>`;
    const icon_ = `<image href="${SIM_IMAGES[`ic_app_home_${icon}.png`]}" x="${iconX}" y="${y - 16}" width="32" height="32"${dark ? ' filter="url(#invert)"' : ""} clip-path="url(#clip-${i})" />`;
    return `<g class="tile${dark ? " dark" : ""}" data-go="${go}">
      <clipPath id="clip-${i}"><polygon points="${tilePoints(row, side)}" /></clipPath>
      <polygon points="${tilePoints(row, side)}" />${icon_}${text}</g>`;
  }).join("");
  const pages = Math.ceil(tiles().length / 6);
  const dots = pages > 1 ? `<div class="dots">${Array.from({ length: pages }, (_, i) => `<i class="${i === state.homePage ? "on" : ""}"></i>`).join("")}</div>` : "";
  return `${bar()}<div class="hero">
    <span class="hh">10</span><i class="slash"></i><span class="mm">48</span>
    <div class="date"><i></i>2026.11.17${state.lang === "zh" ? "<b>十月初八</b>" : ""}<span>${t("tue")}</span></div>
  </div>
  <svg class="tiles" viewBox="0 0 246 206" width="246" height="206">
    <filter id="invert"><feColorMatrix type="matrix" values="-1 0 0 0 1 0 -1 0 0 1 0 0 -1 0 1 0 0 0 1 0" /></filter>
    ${drawn}
  </svg>${dots}`;
}

// 经典首页（非斜切主题）
function classicHome() {
  const start = state.homePage * 6;
  const page = tiles().slice(start, start + 6);
  const cells = page.map(([go, name, icon]) =>
    `<button class="cell ${state.theme}" data-go="${go}"><img src="${SIM_IMAGES[`ic_app_home_${icon}.png`]}" alt="" /><span>${name}</span></button>`).join("");
  const pages = Math.ceil(tiles().length / 6);
  const dots = pages > 1 ? `<div class="dots">${Array.from({ length: pages }, (_, i) => `<i class="${i === state.homePage ? "on" : ""}"></i>`).join("")}</div>` : "";
  const date = state.lang === "zh" ? "11月17日　十月初八" : "11/17";
  return `${bar()}<div class="classic">
    <p class="big-time">10:48</p>
    <p class="meta">${date}　${t("tue")}</p>
    <div class="grid3 cells">${cells}</div>${dots}
  </div>`;
}

function pixelHome() {
  const all = tiles();
  const start = state.homePage * 6;
  const cards = all.slice(start, start + 6).map((item, i) =>
    `<button class="px" data-go="${item[0]}"><i></i><b>${state.lang === "en" ? item[1] : [t("read"), t("walls"), t("ask"), t("notes"), t("cloud"), t("gear"), "buddy", t("shop")][start + i]}</b></button>`).join("");
  const wday = state.lang === "en" ? "Tuesday" : "星期二";
  return `${bar()}<div class="pixel">
    <p class="px-time">10:48</p>
    <p class="px-date">2026/11/17 &nbsp;|&nbsp; ${wday} &nbsp;|&nbsp; ${t("dayNote")}</p>
    <button class="px-card" data-go="detail"><b>${t("recent")}</b><span>三国演义</span><em>33%</em><i></i></button>
    <div class="px-card static"><b>${t("schedule")}</b>
      <span>${t("today")} 01</span><span>${t("doneTab")} 01</span><span>${t("pending")} 02</span></div>
    <div class="px-apps">${cards}</div>
  </div>`;
}

// 阅读应用首页
function detail() {
  const pct = Math.round(((state.page + 1) / pages.length) * 100);
  const days = t("days");
  const bars = [18, 28, 12, 40, 22, 8, 16].map((h) => `<i style="height:${h}%"></i>`).join("");
  const labels = days.map((d) => `<span>${d}</span>`).join("");
  return `${bar()}<div class="bookhome">
    <div class="dash">
      <div class="cont">
        <div>
          <p>${t("readPct")} ${pct}%</p>
          <div class="track"><b style="width:${pct}%"></b><i style="left:${pct}%"></i></div>
        </div>
        <button data-go="reader">${t("continue")}</button>
      </div>
      <div class="week">
        <p>${t("week")}</p>
        <b>42.0 ${t("minutes")}</b>
        <div class="chart">${bars}</div>
        <div class="days">${labels}</div>
      </div>
    </div>
    <div class="stats">
      <div><b>42.0m</b><span>${t("weekDur")}</span></div>
      <div><b>3.5h</b><span>${t("totalDur")}</span></div>
      <div><b>0${t("count")}</b><span>${t("finished")}</span></div>
    </div>
    <p class="recent-h">${t("recent")}</p>
    <div class="recent">
      <button class="cover" data-book="0">三国<br>演义</button>
      <div class="cover empty">${t("noRecent")}</div>
    </div>
    <button class="shelf-btn" data-go="shelf">${t("shelf")} &gt;</button>
  </div>`;
}

const BOOKS = [
  { title: "三国演义", author: "罗贯中", format: "TXT", size: "1.8 MB", seconds: "2.1h", hasText: true },
  { title: "红楼梦", author: "曹雪芹", format: "EPUB", size: "3.4 MB", seconds: "0m", hasText: false },
  { title: "西游记", author: "吴承恩", format: "TXT", size: "1.2 MB", seconds: "18m", hasText: false },
];

function shelf() {
  const toggle = `<div class="seg">
    <button data-shelf="cover" class="${state.shelfView === "cover" ? "on" : ""}">${t("covers")}</button>
    <button data-shelf="list" class="${state.shelfView === "list" ? "on" : ""}">${t("list")}</button></div>`;
  const body = state.shelfView === "cover"
    ? `<div class="shelf-grid">${BOOKS.map((b, i) => `<button class="mini" data-book="${i}"><i>${b.title.slice(0, 2)}</i><span>${b.title}</span></button>`).join("")}</div>`
    : BOOKS.map((b, i) => `<button class="shelf-row" data-book="${i}"><i></i><span><b>${b.title}</b><em>${b.format} · ${b.size}</em></span></button>`).join("");
  return `${bar(t("shelf"))}<div class="shelf"><div class="shelf-head"><b>${t("shelf")}</b>${toggle}</div>${body}<p class="pager">1 / 1</p></div>`;
}

function book() {
  const b = BOOKS[state.bookIndex] || BOOKS[0];
  const pct = b.hasText ? Math.round(((state.page + 1) / pages.length) * 100) : 0;
  const label = pct > 0 ? t("continue") : t("start");
  return `${bar(t("book"))}<div class="book">
    <div class="cover tall">${b.title.slice(0, 2)}<br>${b.title.slice(2)}</div>
    <h3>${b.title}</h3>
    <p>${b.author}</p>
    <div class="meta"><span>${b.format}</span><span>${b.size}</span></div>
    <div class="stats two"><div><b>${b.seconds}</b><span>${t("readTime")}</span></div><div><b>${t("readPct")} ${pct}%</b><span>${t("progress")}</span></div></div>
    <button class="wide" ${b.hasText ? 'data-go="reader"' : "data-nobook"}>${label}</button>
  </div>`;
}

function sheetTabs() { return [t("toc"), t("font"), t("marks"), t("setup")]; }

function pick(items, current, attr) {
  return `<div class="choices">${items.map(([id, name]) =>
    `<button class="choice${current === id ? " on" : ""}" ${attr}="${id}">${name}</button>`).join("")}</div>`;
}

function sheetBody() {
  if (state.sheetTab === 0) return `<button class="row on" data-toc="0">第一回　宴桃园豪杰三结义</button><button class="row" data-toc="1">第二回　张翼德怒鞭督邮</button>`;
  if (state.sheetTab === 1) {
    const fonts = ["MiSans", "Source Han", "Song"].map((name) =>
      `<button class="row${state.fontName === name ? " on" : ""}" data-font="${name}">${name}</button>`).join("");
    return `<p class="note">${t("chooseFont")} · ${state.fontSize}</p>
      <div class="row-btns"><button data-size="-">${t("smaller")}</button><button data-size="+">${t("larger")}</button></div>
      ${fonts}`;
  }
  if (state.sheetTab === 2) {
    const rows = state.bookmarks.length
      ? state.bookmarks.map((p, i) => `<button class="row" data-mark="${i}">${t("book")} ${p + 1}</button>`).join("")
      : `<p class="note">${t("noMarks")}</p>`;
    return `${rows}<button class="choice" data-addmark>${t("addMark")}</button>`;
  }
  const margins = [["narrow", t("narrow")], ["standard", t("standard")], ["wide", t("wide")], ["very", t("veryWide")]];
  const gaps = [["compact", t("compact")], ["standard", t("standard")], ["relaxed", t("relaxed")], ["very", t("veryRelaxed")]];
  return `<p class="note">${t("margin")}</p>${pick(margins, state.margin, "data-margin")}
    <p class="note">${t("spacing")}</p>${pick(gaps, state.spacing, "data-spacing")}
    <p class="note">${state.lang === "en" ? "Paragraph gap" : "段距"}</p>${pick(gaps, state.para, "data-para")}
    <p class="note">${t("turn")}</p>${pick([["slide", t("slide")], ["wipe", t("wipe")]], state.turnStyle, "data-turnstyle")}
    ${shakeSettings(pick, (label, body) => `<p class="note">${label}</p>${body}`)}
    <p class="note">${t("layout")}</p>${pick([["horizontal", t("horizontal")], ["vertical", t("vertical")]], state.writing, "data-writing")}`;
}

function reader() {
  const body = pages[state.page].map((p) => `<p>${glyphs(p)}</p>`).join("");
  const tabs = sheetTabs().map((name, i) =>
    `<span data-sheet="${i}" class="${state.sheetTab === i ? "on" : ""}">${name}</span>`).join("");
  const sheet = state.sheet ? `<div class="sheet"><div class="tabs">${tabs}</div>${sheetBody()}</div>` : "";
  return `<div class="read">
    <div class="read-head"><span>三国演义</span><span>第一回</span></div>
    <div class="read-body">${body}</div>
    <div class="read-foot"><span>10:48</span><span>70%</span><span>${state.page + 1} / ${pages.length}</span></div>
    ${sheet}
  </div>`;
}

function simple(title, lines) {
  return `${bar()}<div class="simple"><h3>${title}</h3>${lines.map((l) => `<p>${l}</p>`).join("")}</div>`;
}

const WALLS = [
  ["山雾", "standby"], ["素纸", ""], ["松", "shutdown"], ["远舟", ""],
  ["空", ""], ["空", ""], ["空", ""], ["空", ""], ["空", ""],
];

function wallpaper() {
  const cells = WALLS.map(([name, use], i) => {
    const tag = use === "standby" ? t("standby") : use === "shutdown" ? t("shutdown") : "";
    return `<button class="wall" data-wall="${i}" ${name === "空" ? "disabled" : ""}><i></i>${tag ? `<em>${tag}</em>` : ""}<span>${name === "空" ? t("empty") : name}</span></button>`;
  }).join("");
  return `${bar()}<div class="walls"><div class="grid3">${cells}</div><p class="pager">1 / 1</p></div>`;
}

function wall() {
  const item = WALLS[state.wallIndex] || WALLS[0];
  const uses = [["shutdown", t("setOff")], ["standby", t("setStandby")], ["reading", t("setReading")]];
  const buttons = uses.map(([id, name]) =>
    `<button class="choice${state.wallUse === id ? " on" : ""}" data-wuse="${id}">${name}</button>`).join("");
  return `${bar()}<div class="wallview">
    <div class="preview"></div>
    <p>${item[0]}.png</p>
    <p class="note">壁纸 · 1.2 MB</p>
    <div class="choices">${buttons}</div>
    <button class="choice" data-wclear>${t("clearSet")}</button>
  </div>`;
}

function tasks() {
  const list = state.todos.filter((t) => state.taskTab === "done" ? t.done : !t.done);
  const rows = list.length
    ? list.map((t) => {
        const i = state.todos.indexOf(t);
        return `<button class="todo" data-task="${i}"><i class="${t.done ? "on" : ""}"></i><span>${esc(t.text)}</span></button>`;
      }).join("")
    : `<p class="empty">${state.taskTab === "done" ? t("noDone") : t("noTodo")}</p>`;
  const open = state.taskOpen >= 0 ? state.todos[state.taskOpen] : null;
  const dialog = open ? `<div class="mask"><div class="dialog"><p>${t("choose")}</p>
      ${open.done ? "" : `<button data-task-do="done">${t("complete")}</button>`}
      <button data-task-do="del">${t("deleteTodo")}</button></div></div>` : "";
  return `${bar()}<div class="tasks">
    <div class="seg pill">
      <button data-ttab="todo" class="${state.taskTab === "todo" ? "on" : ""}">${t("todo")}</button>
      <button data-ttab="done" class="${state.taskTab === "done" ? "on" : ""}">${t("done")}</button>
    </div>
    <div class="thead"><i>★</i><b>${state.taskTab === "done" ? t("done") : t("schedule")}</b></div>
    <div class="frame">${rows}<div class="frame-foot"><span>1 / 1</span><button data-trefresh>${t("refresh")}</button></div></div>
    ${dialog}
  </div>`;
}

const PUSH = [["三国演义.txt", "书籍"], ["山雾.png", "壁纸"], ["misans_30_2.ef", "字体"]];

function transfer() {
  const tabs = [["local", L("本地服务", "Local")], ["app", t("appPush")], ["ap", t("ap")], ["wifi", t("wifiXfer")], ["usb", t("usbXfer")]]
    .map(([id, name]) => `<button data-ctab="${id}" class="${state.cloudTab === id ? "on" : ""}">${name}</button>`).join("");
  let body = "";
  if (state.cloudTab === "local") {
    const running = state.localService;
    body = `<div class="local-service-card ${running ? "on" : ""}">
      <div class="local-service-head"><b>${L(running ? "本地传输已开启" : "本地传输", running ? "Local transfer on" : "Local transfer")}</b></div>
      <p>${L("用手机或电脑上传书籍、字库和壁纸。", "Upload books, fonts and wallpapers from your phone or computer.")}</p>
      ${running ? `<code>${esc(state.localServiceUrl)}</code><p>${state.localServiceMode === "ap" ? L(`热点：${state.localServiceSsid} · 密码：${state.localServicePassword}`, `Hotspot: ${state.localServiceSsid} · Password: ${state.localServicePassword}`) : L("连接 Metalio-Home，在同一 Wi-Fi 下打开。", "Join Metalio-Home and open this address.")}</p>` : `<p>${L("优先使用当前 Wi-Fi；只有 4G 时，请连接 Wi-Fi 或使用临时热点。", "Use your current Wi-Fi, or start a temporary hotspot.")}</p>`}
      <button class="choice" data-local-service>${running ? L("关闭本地传输", "Stop local transfer") : L("开启本地传输", "Start local transfer")}</button>
    </div>`;
  } else if (state.cloudTab === "app") {
    const filters = [["all", t("all")], ["book", t("books2")], ["wall", t("walls2")], ["font", t("fonts")]]
      .map(([id, name]) => `<button data-cfilter="${id}" class="${state.cloudFilter === id ? "on" : ""}">${name}</button>`).join("");
    const rows = PUSH.filter(([, kind]) => state.cloudFilter === "all" || (state.cloudFilter === "book" && kind === "书籍") || (state.cloudFilter === "wall" && kind === "壁纸") || (state.cloudFilter === "font" && kind === "字体"))
      .map(([name, kind]) => `<div class="app-row"><i></i><span><b>${name}</b><em>${kind === "书籍" ? t("books2") : kind === "壁纸" ? t("walls2") : t("fonts")}</em></span><button>${t("saveLocal")}</button></div>`).join("");
    body = `<div class="thead"><b>${t("incoming")}</b><span data-crefresh>${t("refresh")}</span></div><div class="seg tiny">${filters}</div><div class="frame">${rows || `<p class="empty">${t("nothing")}</p>`}</div>`;
  } else if (state.cloudTab === "ap") {
    body = `<div class="step"><b>${t("step1")}</b><p>${t("join")}</p><div class="qr"><span>QR</span></div><p>WiFi: Metalio-EINK</p></div>
      <div class="step"><b>${t("step2")}</b><p>${t("openAddr")}</p><p><code>http://192.168.4.1</code></p><p class="note">${t("orScan")}</p></div>`;
  } else if (state.cloudTab === "wifi") {
    body = state.link === "4G" ? `<p class="empty">${t("needWifi")}</p>` : `<p class="empty">${t("preparing")}</p>`;
  } else {
    body = `<div class="step"><b>${t("method")}</b><p>${state.usb ? t("usbOnHint") : t("usbOffHint")}</p>
      <button class="choice" data-usb>${state.usb ? t("usbOn") : t("usbOff")}</button></div>`;
  }
  return `${bar()}<div class="xfer"><div class="seg wrap">${tabs}</div>${body}</div>`;
}

function buddy() {
  if (!state.buddyBound) {
    return `${bar("workbuddy")}<div class="buddy">
      <p>${t("scanLogin")}</p>
      <div class="qr"><span>QR</span><b>5 min</b></div>
      <p class="note">${t("scanHint")}</p>
      <button class="choice" data-buddy-ok>${t("pretend")}</button>
    </div>`;
  }
  const menu = state.buddyMenu ? `<div class="wb-menu"><button data-buddy-sync>${t("sync")}</button><button data-buddy-relink>${t("relink")}</button></div>` : "";
  return `${bar("workbuddy")}<div class="buddy chat">
    <div class="wb-top"><b>workbuddy (${t("online")})</b><button data-buddy-menu>⋯</button></div>
    ${menu}
    <p><b>${t("assistant")}</b>${t("ready")}</p>
    <p class="note">${t("todayWork")}</p>
    <p class="note hold">${t("hold")}</p>
  </div>`;
}

function store() {
  const apps = [["阅读计时", "1.0.2", false, "tool"], ["词典", "0.4.0", false, "reading"], ["天气", "1.2.1", true, "tool"]]
    .map(([name, ver, on, cat]) => `<button class="app-row" data-app="${name}"><i></i><span><b>${name}</b><em>${t(cat)} · ${ver}</em></span><em class="st">${on ? t("installed") : t("notInstalled")}</em></button>`).join("");
  const detail = state.appName ? `<div class="mask"><div class="dialog">
      <b>${state.appName}</b><p class="note">当前版本见列表。假壳子不会下载。</p>
      <button>${t("install")}</button><button data-app-close>${t("close")}</button></div></div>` : "";
  return `${bar(t("store"))}<div class="store">
    <div class="thead"><b>${t("allApps")}</b><span>${t("filter")}</span></div>
    <div class="frame">${apps}</div>${detail}</div>`;
}

const WIFI = [{ ssid: "Metalio-Home", saved: true }, { ssid: "Cafe", saved: false }, { ssid: "Guest", saved: false }];

function wifi() {
  const saved = WIFI.filter((w) => w.saved);
  const fresh = WIFI.filter((w) => !w.saved);
  const row = (w) => `<div class="app-row"><span><b>${w.ssid}</b></span>${w.saved ? `<button>${t("pin")}</button><button data-wdel="${w.ssid}">${t("del")}</button>` : `<button data-wjoin="${w.ssid}">${t("connect")}</button>`}</div>`;
  return `${bar(t("wifi"))}<div class="wifi">
    <div class="thead"><b>${t("saved")} (${saved.length})</b><button data-wscan>${t("refresh2")}</button></div>
    <div class="frame">${saved.map(row).join("") || `<p class="empty">${t("noneSaved")}</p>`}</div>
    <div class="thead"><b>${t("nearby")} (${fresh.length})</b></div>
    <div class="frame">${fresh.map(row).join("") || `<p class="empty">${t("noneNearby")}</p>`}</div>
  </div>`;
}

function pager() {
  return `${bar(t("pager"))}<div class="pager-page">
    <h3>${t("autoPager")}</h3>
    <p class="cur">${t("enabled")}</p>
    <p class="note">${t("autoHint")}</p>
    <h3>${t("current")}</h3>
    <p class="cur">${state.pagerOn ? "PageTurner-A · " + t("connected") : t("notConnected")}</p>
    <div class="thead"><b>${t("savedDev")} (1)</b><button data-pscan>${t("refresh2")}</button></div>
    <div class="app-row"><span><b>PageTurner-A</b><em>${state.pagerOn ? t("connected") : t("away")}</em></span><button data-pconn>${state.pagerOn ? t("disconnect") : t("connect")}</button></div>
    <p class="note">${t("noNew")}</p>
  </div>`;
}

function standbyScreen() {
  if (state.standbyStyle === "wallpaper") return `<div class="standby wallbg"><p>10:48</p><span>${t("tue")}</span></div>`;
  const todo = state.todos.find((item) => !item.done);
  return `<div class="standby classic">
    <p class="hm">10:48</p>
    <p class="date">${state.lang === "zh" ? "11月17日　十月初八" : "Nov 17"}　${t("tue")}</p>
    <div class="wx"><b>${t("weather")}</b><span>${t("temp")}</span></div>
    <button class="todo-line" data-go="tasks"><b>${t("standbyTodo")}</b><span>${todo ? esc(todo.text) : t("noTodo")}</span></button>
  </div>`;
}
