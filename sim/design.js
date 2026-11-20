// 新设计（随行）。和原始设计共用状态、按键、各应用的数据；只换首页、助手、正文、设置的画法。
// 原始设计的函数（home、ai、reader、settings…）不动，页头的页签切 state.design。
const v2 = { solo: blankTalk(), notes: {}, pending: null, direct: null };

function blankTalk() {
  return { q: "", a: [], p: 0 };
}
function isV2() {
  return state.design === "v2";
}
function L(zh, en) {
  return state.lang === "en" ? en : zh;
}
// 「自建」填好网关和配对码才算连上。
function agentReady() {
  return state.paired && /^wss?:\/\/.+/.test(state.gateway) && state.pair.trim() !== "";
}
// 通话只属于自己的助理：没选「自建」或还没配对时，哪里都不出现通话入口。
function callEntry() {
  return state.provider === "agent" && agentReady();
}
function onBook() {
  return state.provider === "agent" && state.sessionType === "book";
}
// 当前这一场。带书时每一页各有一条「注」，不带书是单独一场。
function nowTalk() {
  if (onBook()) return (v2.notes[state.fromPage] ||= blankTalk());
  return v2.solo;
}

// 图标。线宽和圆头照官方首页那六个 90 px 图标画。
const V2_ICONS = {
  mic: '<rect x="9" y="2.5" width="6" height="11.5" rx="3"/><path d="M5 11.5a7 7 0 0 0 14 0M12 18.5v3"/>',
  micOff: '<rect x="9" y="2.5" width="6" height="11.5" rx="3"/><path d="M5 11.5a7 7 0 0 0 14 0M12 18.5v3M4 3l16 18"/>',
  wave: '<path d="M3.5 10v4M7.75 6.5v11M12 3v18M16.25 6.5v11M20.5 10v4"/>',
  book: '<path d="M12 6.5C10 4.6 6.5 4 3 4.5v14c3.5-.5 7 .1 9 2 2-1.9 5.5-2.5 9-2v-14c-3.5-.5-7 .1-9 2zM12 6.5v14"/>',
  back: '<path d="M14.5 5l-7 7 7 7"/>',
  go: '<path d="M9.5 5l7 7-7 7"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  x: '<path d="M6 6l12 12M18 6L6 18"/>',
  pause: '<path d="M8.5 5v14M15.5 5v14"/>',
  cam: '<path d="M3.5 8h3.2l1.6-2.5h7.4L17.3 8h3.2v11h-17z"/><circle cx="12" cy="13.2" r="3.2"/>',
  speaker: '<path d="M4 9.5v5h3.5l5 4v-13l-5 4zM16 9a4.2 4.2 0 0 1 0 6M18.6 6.4a7.8 7.8 0 0 1 0 11.2"/>',
  net: '<path d="M4.5 20v-3M9.5 20v-7M14.5 20V9M19.5 20V4"/>',
  sun: '<circle cx="12" cy="12" r="3.6"/><path d="M12 2.8v2.4M12 18.8v2.4M2.8 12h2.4M18.8 12h2.4M5.5 5.5l1.7 1.7M16.8 16.8l1.7 1.7M5.5 18.5l1.7-1.7M16.8 7.2l1.7-1.7"/>',
  device: '<rect x="6.5" y="2.5" width="11" height="19" rx="2"/><path d="M10 18h4"/>',
  shelf: '<path d="M5 4v15.5M9.5 4v15.5M13.5 5.5l4.5 14M3 20h18"/>',
  buddy: '<path d="M4 4.5h16v11.5h-9l-4.5 4v-4H4z"/><path d="M8.5 10.3h.01M12 10.3h.01M15.5 10.3h.01"/>',
  store: '<path d="M5 8.5h14l-1 12H6zM9 8.5V7a3 3 0 0 1 6 0v1.5"/>',
};
function ic(name, cls = "") {
  return `<svg class="ic${cls ? " " + cls : ""}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${V2_ICONS[name]}</svg>`;
}

function v2Head(title, sub = "", right = "") {
  return `<header class="v-head"><button class="v-back" data-back aria-label="${L("返回", "Back")}">${ic("back")}</button>
    <div class="v-title"><b>${title}</b>${sub ? `<span>${sub}</span>` : ""}</div><div class="v-right">${right}</div></header>`;
}

// ── 可以问的话（假数据）。一项是 [按钮上的字, 问的那句, 回答分页, 顺带做的事]。
function v2Sugs() {
  if (state.provider !== "agent") {
    return [
      ["这一页在讲什么？", "这一页在讲什么？", ["我不知道你在读哪一页。把这段念给我听，我再帮你看。"]],
      ["三国演义从哪里开始讲？", "三国演义从哪里开始讲？", ["开篇从「分久必合，合久必分」讲起。"]],
      ["明天出门要带伞吗？", "明天出门要带伞吗？", ["明天多云转小雨，18 到 22 度。", "下午三点以后有雨，出门带把伞。"]],
    ];
  }
  if (state.sessionType !== "book") {
    return [
      ["记一条待办", "帮我记一下，周六去取装裱的字。", ["记下了：周六去取装裱的字。", "已经放进每日清单，首页的待办多了一条。"], () => {
        if (!state.todos.some((x) => x.text === "周六去取装裱的字")) state.todos.unshift({ text: "周六去取装裱的字", done: false });
      }],
      ["明天出门要带伞吗？", "明天出门要带伞吗？", ["明天多云转小雨，18 到 22 度。", "下午三点以后有雨，出门带把伞。"]],
      ["安排一个轻松的周末", "帮我安排一个轻松的周末。", ["周六留给户外：早上散步，下午找一家喜欢的咖啡馆。", "周日留半天读书，晚上把下一周的安排理一遍。"]],
    ];
  }
  const what = ["这一页在讲什么？", "这一页在讲什么？", answers[state.fromPage]];
  return [
    [
      what,
      ["「分久必合」是什么意思？", "「分久必合，合久必分」是什么意思？", ["天下分裂久了会统一，统一久了又会分裂。", "作者把它放在全书第一句，后面一百二十回都照着这句话走。"]],
      ["周末七国是哪七国？", "周末七国是哪七国？", ["这里的「周末」是周朝末年。七国是齐、楚、燕、韩、赵、魏、秦。", "后来秦灭了另外六国，所以说「并入于秦」。"]],
    ],
    [
      what,
      state.fontpack
        ? ["「蟠」字怎么读？", "「蟠」字怎么读？", ["读 pán，盘曲、环绕的意思。", "「蟠于椅上」就是蛇盘在椅子上。"]]
        : ["方框里是什么字？", "正文里那个方框是什么字？", ["读 pán，「盘」的意思：盘曲、环绕。这一句是说蛇盘在椅子上。", "机内的主字库没有这个字形，所以正文和我这里都只能画成方框。把扩展字库放到 SD 卡上，它就能显示。"]],
      ["温德殿是什么地方？", "温德殿是什么地方？", ["温德殿是洛阳皇宫里的一座殿，汉灵帝在这里上朝。", "书里让怪事出在御座上，是说朝廷要出事。"]],
    ],
    [
      what,
      ["雌鸡化雄是什么意思？", "雌鸡化雄是什么意思？", ["母鸡变成了公鸡。古人把它看成阴阳颠倒的凶兆。", "后文蔡邕上疏，说这是妇人和宦官干政招来的。"]],
      ["光和元年是哪一年？", "光和元年是哪一年？", ["光和是汉灵帝的年号。光和元年是公元 178 年。"]],
    ],
  ][state.fromPage];
}
function v2Lines() {
  if (onBook()) return liveLines[state.fromPage];
  return ["我在。想聊点什么？", "可以慢慢说，我听着。"];
}

// ── 进出助手页
function v2Load() {
  const s = nowTalk();
  state.answered = s.a.length > 0;
  state.answerPage = s.p;
}
function v2EnterAi(from) {
  state.aiBack = from;
  state.fromPage = state.page; // 正在读的那一页，随时可以带上
  state.sessionType = state.provider === "agent" && from === "reader" ? "book" : "standalone";
  state.liveMuted = false;
  v2Load();
  if (state.provider !== "agent") {
    log(`进入提问（${PROVIDERS[state.provider]}）。${from === "reader" ? "官方这条不带正在读的那一页。" : ""}`);
  } else if (state.sessionType === "book") {
    log(`带上《三国演义》第 ${state.page + 1} 页进入提问。${state.answered ? "这一页问过，那条注还在。" : ""}`);
  } else {
    log("进入随口问。不带书。");
  }
}
function v2LeaveAi() {
  nowTalk().p = state.answerPage;
}
// 松手发送前：把这次问的那句和回答放进当前这一场。
function v2Commit() {
  if (state.provider === "agent" && !agentReady()) {
    toast(L("还没连上你的助手", "Assistant not connected"));
    return false;
  }
  const list = v2Sugs();
  const s = nowTalk();
  let i = v2.pending;
  if (i === null) i = (list.findIndex((x) => x[1] === s.q) + 1) % list.length;
  v2.pending = null;
  const [, q, a, effect] = list[i];
  Object.assign(s, { q, a, p: 0 });
  if (effect) effect();
  if (state.photo) {
    state.photo = false;
    log("照片和这句话一起发出去了。");
  }
  return true;
}
function v2SwitchSubject(type) {
  if (state.provider !== "agent" || type === state.sessionType) return;
  nowTalk().p = state.answerPage;
  const live = state.live;
  stopSession();
  state.liveMuted = false;
  state.sessionType = type;
  v2Load();
  if (live) startLive();
  log(type === "book" ? `换到「这一页」：带上第 ${state.fromPage + 1} 页。随口问那一场留着。` : "换到「随口问」：不带书。这一页的注留着。");
  render();
}
function v2Ask() {
  if (state.screen !== "ai") return go("ai");
  if (state.live) switchMode("ask");
}
function v2GoLive() {
  if (state.screen !== "ai") go("ai");
  if (state.provider !== "agent") return toast(L("通话要用「自建」", "Calls need Custom"));
  if (!agentReady()) return render();
  if (!assistantCaps().live) return toast(L("这个助理还没配置通话", "This assistant has no Call set up"));
  if (!state.live) switchMode("live");
}
function openSetting(id) {
  if (state.screen === "settings") {
    state.settingsScreen = id;
    return render();
  }
  go("settings");
  state.settingsScreen = id;
  v2.direct = id;
  render();
}
const V2_PARENT = { power: "device", storage: "device", lang: "device", diag: "device", about: "device" };
// 返回 true = 这次返回已经在设置里往上走了一级。
function v2Back() {
  if (state.screen !== "settings") return false;
  if (state.settingsScreen === "talk" && state.assistantDetail) {
    if (state.assistantUnpair) state.assistantUnpair = false;
    else state.assistantDetail = null;
    render();
    return true;
  }
  if (state.settingsScreen === "talk" && (state.assistantSetup || state.assistantUnpair || state.assistantAdvanced)) {
    if (state.assistantSetup) SetupPage.reset();
    state.assistantSetup = false;
    state.assistantUnpair = false;
    state.assistantAdvanced = false;
    state.localService = false;
    render();
    return true;
  }
  const at = state.settingsScreen;
  // 在总览，或者是从别的页面直接跳进这一项的：回到来的地方
  if (at === "list" || at === v2.direct) return false;
  state.settingsScreen = V2_PARENT[at] || "list";
  render();
  return true;
}
// 灰键：首页两屏之间翻。
function settingsPaneEl() {
  return document.querySelector(".v-pane, .set .pane");
}
function scrollSettings(dir) {
  const el = settingsPaneEl();
  if (!el || el.scrollHeight <= el.clientHeight + 8) return false;
  const step = Math.max(72, Math.round(el.clientHeight * 0.72));
  const next = Math.max(0, Math.min(el.scrollHeight - el.clientHeight, el.scrollTop + dir * step));
  if (next === el.scrollTop) {
    log(dir < 0 ? "已经到这一页的顶。" : "已经到这一页的底。");
    return true;
  }
  el.scrollTop = next;
  return true;
}

function v2Chin(dir) {
  if (state.screen !== "home") return false;
  const n = Math.min(1, Math.max(0, state.homePage + dir));
  if (n === state.homePage) {
    log(n === 0 ? "已经是首页第一屏。" : "已经是最后一屏。");
    return true;
  }
  state.homePage = n;
  log(n === 0 ? "灰键：回到首页第一屏。" : "灰键：翻到全部应用。");
  flash();
  render();
  return true;
}

// ── 首页
function v2Who() {
  if (state.provider !== "agent") return `${PROVIDERS[state.provider]} · ${L("官方订阅", "official plan")}`;
  const current = state.assistants.find((item) => item.id === state.activeAssistant);
  return agentReady() ? `${current?.name || L("自建", "Custom")} · ${state.gatewayKind}` : L("自建 · 还没配对", "Custom · not paired");
}
function v2Dots() {
  return `<span class="dots">${[0, 1].map((i) => `<i class="${i === state.homePage ? "on" : ""}"></i>`).join("")}</span>`;
}
function v2Home() {
  if (state.homePage > 0) return v2Apps();
  const agent = state.provider === "agent";
  const liveOk = agent && agentReady() && assistantCaps().live;
  const open = state.todos.filter((x) => !x.done);
  const pct = Math.round(((state.page + 1) / pages.length) * 100);
  // 有自己的助理：提问和通话两块。没有：只有提问，占满整行，斜边留在右侧。
  const pairArt = callEntry()
    ? `<polygon class="dark" data-ask points="0.6,0.6 127,0.6 96,133.4 0.6,133.4" />
        <polygon class="${liveOk ? "" : "off"}" data-live points="134,0.6 221.4,0.6 221.4,133.4 103,133.4" />`
    : `<polygon class="dark" data-ask points="0.6,0.6 221.4,0.6 190.4,133.4 0.6,133.4" />`;
  const callBlock = callEntry()
    ? `<div class="v-call${liveOk ? "" : " off"}">${ic("wave")}<b>${L("通话", "Call")}</b><span>${liveOk ? L("接通就聊", "Just talk") : L("还没配置通话", "Call not set up")}</span></div>`
    : "";
  return `${bar()}<main class="v-home">
    <section class="v-hero">
      <div class="v-clock"><span>10</span><i></i><span>48</span></div>
      <div class="v-date"><i></i><span>2026.11.17</span><b>${L("十月初八", "November 17")}</b><span>${t("tue")} · ${t("weather")} ${t("temp")}</span></div>
    </section>
    <section class="v-pair">
      <svg viewBox="0 0 222 134" width="222" height="134">
        <pattern id="v-dither" width="3" height="3" patternUnits="userSpaceOnUse"><rect width="1" height="1" fill="#111"/></pattern>
        ${pairArt}
      </svg>
      <div class="v-ask">${ic("mic")}<b>${L("提问", "Ask")}</b><span>${L("按住，说一句", "Hold and say it")}</span></div>
      ${callBlock}
    </section>
    <button class="v-who" data-setting="talk"><i class="${!agent || liveOk ? "on" : ""}"></i><span>${v2Who()}</span>${ic("go")}</button>
    <button class="v-book" data-go="reader">
      <span class="v-cover">三国<br>演义</span>
      <span class="v-book-main"><b>三国演义</b><em>${L("第一回 · 第", "Ch. 1 · p.")} ${state.page + 1} / ${pages.length} ${L("页", "")}</em><i class="v-track"><i style="width:${pct}%"></i></i></span>
      <span class="v-wedge">${L("接着读", "Read")}${ic("go")}</span>
    </button>
    <button class="v-todo" data-go="tasks"><b>${open.length}</b><span>${open.length ? esc(open[0].text) : L("今天没有待办", "Nothing to do today")}</span>${open.length > 1 ? `<em>${L(`还有 ${open.length - 1} 条`, `+${open.length - 1}`)}</em>` : ""}${ic("go")}</button>
    <footer class="v-foot">${v2Dots()}<button data-homepage="1">${L("全部应用", "All apps")}${ic("go")}</button></footer>
  </main>`;
}

function v2AppList() {
  return [
    ["detail", L("阅读", "Reading"), "book"], ["shelf", L("书架", "Shelf"), "@shelf"],
    ["tasks", t("todos"), "task"], ["transfer", t("cloud"), "cloud"],
    ["buddy", "workbuddy", "@buddy"], ["wallpaper", t("walls"), "wallpaper"],
    ["store", t("shop"), "@store"], ["settings", t("gear"), "setting"],
  ];
}
// 第二屏：官方的斜切应用格，四排。设置 → 显示里的样式只管这一屏。
function v2Apps() {
  const list = v2AppList();
  const foot = `<footer class="v-foot">${v2Dots()}<button data-homepage="0">${ic("back")}${L("今天", "Today")}</button></footer>`;
  const theme = state.theme === "pixel" ? "slash" : state.theme;
  if (theme !== "slash") {
    const cells = list.map(([go, name, icon]) => {
      const img = icon[0] === "@" ? ic(icon.slice(1)) : `<img src="${SIM_IMAGES[`ic_app_home_${icon}.png`]}" alt="" />`;
      return `<button class="v-cell ${theme}" data-go="${go}">${img}<span>${name}</span></button>`;
    }).join("");
    return `${bar()}<main class="v-apps"><h2>${L("全部应用", "All apps")}</h2><div class="v-cells">${cells}</div>${foot}</main>`;
  }
  const H = 74;
  const drawn = list.map(([go, name, icon], i) => {
    const row = Math.floor(i / 2);
    const side = i % 2;
    const dark = (row + side) % 2 === 0;
    const y0 = row * 82 + 0.6;
    const y1 = y0 + H;
    const [top, bottom] = row % 2 ? [96, 140] : [140, 96];
    const points = side === 0 ? `12.6,${y0} ${top},${y0} ${bottom},${y1} 12.6,${y1}` : `${top + 7},${y0} 233.4,${y0} 233.4,${y1} ${bottom + 7},${y1}`;
    const cy = y0 + H / 2;
    const ix = side === 0 ? (name.length > 5 ? 17 : 23) : 195;
    const art = icon[0] === "@"
      ? `<svg x="${ix}" y="${cy - 16}" width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${V2_ICONS[icon.slice(1)]}</svg>`
      : `<image href="${SIM_IMAGES[`ic_app_home_${icon}.png`]}" x="${ix}" y="${cy - 16}" width="32" height="32"${dark ? ' filter="url(#invert)"' : ""} />`;
    const long = name.length > 5 ? ' class="long"' : "";
    const text = side === 0
      ? `<text${long} x="${ix + 37}" y="${cy + 4.5}">${name}</text>`
      : `<text${long} x="${ix - 6}" y="${cy + 4.5}" text-anchor="end">${name}</text>`;
    return `<g class="tile${dark ? " dark" : ""}" data-go="${go}"><polygon points="${points}" />${art}${text}</g>`;
  }).join("");
  return `${bar()}<main class="v-apps"><h2>${L("全部应用", "All apps")}</h2>
    <svg class="tiles" viewBox="0 0 246 324" width="246" height="324">
      <filter id="invert"><feColorMatrix type="matrix" values="-1 0 0 0 1 0 -1 0 0 1 0 0 -1 0 1 0 0 0 1 0" /></filter>${drawn}
    </svg>${foot}</main>`;
}

// ── 助手页：提问和通话是同一页的两种说法；「这一页 / 随口问」是谈哪件事。
function v2Welcome(book) {
  const sugs = v2Sugs().map(([label], i) => `<button data-sug="${i}"><i>${i + 1}</i><span>${label}</span>${ic("go")}</button>`).join("");
  const h = book ? L("问这一页", "Ask about this page") : L("想问什么？", "What's on your mind?");
  const p = state.provider !== "agent"
    ? L("按住说话，松开发送。", "Hold to talk, release to send.")
    : book ? L("正文已经带上，不用念给我听。", "The page is attached.") : L("不带书。待办、天气，随便聊。", "No book. Tasks, weather, anything.");
  return `<div class="v-welcome"><h2>${h}</h2><p>${p}</p><div class="v-sugs">${sugs}</div></div>`;
}
function v2Answer(s) {
  const n = Math.min(state.answerPage, s.a.length - 1);
  return `<div class="v-qa"><p class="q"><i>${L("问", "Q")}</i>${glyphs(esc(s.q))}</p><p class="a">${glyphs(esc(s.a[n]))}</p></div>`;
}
function v2LiveBody() {
  const say = v2Lines();
  // 字幕整页换。AI 说完后上一句留在屏上，只刷上面那条状态。
  const text = state.caption || (state.liveIndex > 0 ? say[(state.liveIndex - 1) % say.length] : "");
  const st = state.speaking
    ? ["is-speak", L("在说", "Speaking"), state.liveMuted ? L("麦克风关着，这段照常放完", "Mic is off") : L("短按 AI 唤醒键打断", "Short-press AI key to interrupt")]
    : state.liveMuted
      ? ["is-mute", L("静音", "Muted"), L("麦克风关着，助手听不到", "The assistant cannot hear you")]
      : ["is-listen", L("在听", "Listening"), L("直接说，不用按键", "Just speak")];
  return `<div class="v-live"><div class="v-state ${st[0]}"><span class="v-bars">${"<i></i>".repeat(7)}</span><b>${st[1]}</b><em>${st[2]}</em></div>
    <p class="v-caption${state.speaking ? "" : " past"}">${text ? glyphs(esc(text)) : L("接通了。想到什么就说。", "Connected. Say anything.")}</p></div>`;
}
function v2Ai() {
  const agent = state.provider === "agent";
  const ready = !agent || agentReady();
  const book = onBook();
  const s = nowTalk();
  const who = agent ? v2Who() : `${PROVIDERS[state.provider]} AI`;
  const fresh = s.a.length && !state.listening && !state.live ? `<button class="v-txt" data-new>${L("新对话", "New")}</button>` : "";
  let top = "";
  if (agent) {
    top = `<div class="v-subject ${book ? "at-book" : "at-solo"}">
      <button data-subject="book" class="${book ? "on" : ""}">${ic("book")}${L("这一页", "This page")}</button>
      <button data-subject="standalone" class="${book ? "" : "on"}">${L("随口问", "Anything")}</button></div>`;
    if (book) {
      top += `<button class="v-quote" data-quote><span>三国演义 · 第一回 · ${L("第", "p.")} ${state.fromPage + 1} ${L("页", "")}</span>
        <p>${glyphs(esc(pages[state.fromPage][0]))}</p></button>`;
    }
  } else if (state.aiBack === "reader") {
    top = `<button class="v-warn" data-setting="talk"><span>${PROVIDERS[state.provider]}${L("拿不到正在读的这一页。换成「自建」才会带上。", " cannot see the page you are reading.")}</span>${ic("go")}</button>`;
  }
  let body;
  if (!ready) {
    body = `<div class="v-setup"><h2>${L("还没连上你的助手", "Assistant not connected")}</h2><p>${L("填上网关地址和配对码，提问和通话都走你自己的 OpenClaw 或 Hermes。", "Add the gateway address and pairing code.")}</p>
      <button class="v-solid" data-setting="talk">${L("去连接", "Connect")}${ic("go")}</button></div>`;
  } else if (state.live) body = v2LiveBody();
  else if (state.listening) body = `<div class="v-listen">${ic("mic")}<b>${L("在听", "Listening")}</b><span>${L("松开就发出去", "Release to send")}</span></div>`;
  else if (s.a.length) body = v2Answer(s);
  else body = v2Welcome(book);

  let foot = "";
  if (ready && state.live) {
    foot = `<div class="v-callbar">
      <button data-mute class="${state.liveMuted ? "on" : ""}">${ic(state.liveMuted ? "micOff" : "mic")}${state.liveMuted ? L("取消静音", "Unmute") : L("静音", "Mute")}</button>
      <button data-interrupt ${state.speaking ? "" : "disabled"}>${ic("pause")}${L("打断", "Interrupt")}</button>
      <button class="end" data-mode="ask">${ic("x")}${L("挂断", "End")}</button></div>`;
  } else if (ready) {
    const pager = s.a.length && !state.listening
      ? `<div class="v-pages"><button data-answer="-1" ${state.answerPage > 0 ? "" : "disabled"}>${ic("back")}</button><span>${state.speaking ? L("在说 · 短按 AI 唤醒键停下", "Speaking") : `${state.answerPage + 1} / ${s.a.length}`}</span><button data-answer="1" ${state.answerPage < s.a.length - 1 ? "" : "disabled"}>${ic("go")}</button></div>`
      : "";
    const cam = state.provider === "baiwen" || agent
      ? `<button class="v-cam${state.photo ? " on" : ""}" data-v2photo aria-label="${L("拍照", "Photo")}">${ic("cam")}</button>` : `<i></i>`;
    const holdWord = state.listening ? L("松开发送", "Release to send") : state.photo ? L("按住说话，连照片一起发", "Hold to talk, photo attached") : L("按住说话", "Hold to talk");
    foot = `${pager}<div class="v-talk${state.listening ? " rec" : ""}">${cam}<span data-hold>${holdWord}</span>
      ${callEntry() ? `<button class="tolive" data-live>${ic("wave")}${L("通话", "Call")}</button>` : ""}</div>`;
  }
  return `${bar()}<main class="v-page v-ai">${v2Head(state.live ? L("通话", "Call") : L("提问", "Ask"), who, fresh)}${top}
    <div class="v-body">${body}</div>${foot}</main>`;
}

// ── 正文：页面上不放按钮。问过的页有「注」，缺字形的页在页脚提示。
function v2Reader() {
  const agent = state.provider === "agent";
  const note = v2.notes[state.page];
  const marked = agent && note && note.a.length > 0;
  const miss = missingGlyphs(state.page);
  const body = pages[state.page].map((p) => `<p>${glyphs(p)}</p>`).join("");
  const tabs = sheetTabs().map((name, i) => `<span data-sheet="${i}" class="${state.sheetTab === i ? "on" : ""}">${name}</span>`).join("");
  const pack = state.sheetTab === 1
    ? `<button class="v-pack" data-setting="read"><span>${L("扩展字库", "Extended fonts")}：${state.fontpack ? L("已安装", "installed") : L(miss ? `未安装，这一页缺 ${miss} 个字` : "未安装", "not installed")}</span>${ic("go")}</button>` : "";
  const sheet = state.sheet ? `<div class="sheet v-sheet">
      <div class="v-sheet-ai">
        <button class="solid" data-ask>${ic("mic")}${agent ? L("问这一页", "Ask this page") : `${L("问", "Ask ")}${PROVIDERS[state.provider]}`}</button>
        ${callEntry() ? `<button data-live>${ic("wave")}${L("聊这一页", "Talk about it")}</button>` : ""}
      </div>
      <div class="tabs">${tabs}</div>${pack}${sheetBody()}</div>` : "";
  return `<div class="read v-read">
    <div class="read-head"><span>三国演义 · 第一回</span>${marked ? `<button class="v-mark" data-ask>${L("注", "Note")}</button>` : ""}</div>
    <div class="read-body">${body}</div>
    <div class="read-foot"><span>10:48</span>${miss ? `<button class="v-miss" data-fontfix>${L(`缺 ${miss} 个字`, `${miss} missing`)}</button>` : `<span>${state.battery}%</span>`}<span>${state.page + 1} / ${pages.length}</span></div>
    ${sheet}</div>`;
}

// ── 设置：按「我想做什么」分六项，每项直接写着现在的值。不滚动。
function v2Seg(items, current, attr) {
  return `<div class="v-seg">${items.map(([id, name]) => `<button ${attr}="${id}" class="${String(current) === String(id) ? "on" : ""}">${name}</button>`).join("")}</div>`;
}
function v2Fld(label, body, note = "") {
  return `<div class="v-fld"><label>${label}</label>${body}${note ? `<p>${note}</p>` : ""}</div>`;
}
function v2Level(icon, label, value, key) {
  const n = Math.round(value / 10);
  const blocks = Array.from({ length: 10 }, (_, i) => `<button data-level="${key}:${i + 1}" class="${i < n ? "on" : ""}" aria-label="${label} ${(i + 1) * 10}"></button>`).join("");
  return `<div class="v-level">${ic(icon)}<span>${label}</span><div class="v-blocks">${blocks}</div><b>${value}</b></div>`;
}
function v2Row(id, icon, name, value) {
  return `<button class="v-row" data-setting="${id}">${icon ? ic(icon) : ""}<span><b>${name}</b><em>${value}</em></span>${ic("go", "chev")}</button>`;
}
const LIGHT = { cool: ["冷光", "Cool"], warm: ["暖光", "Warm"], both: ["冷暖光", "Mixed"], off: ["关", "Off"] };
const REFRESH = { clear: ["清晰", "Clear"], balanced: ["均衡", "Balanced"], fast: ["快速", "Fast"] };
const BT = { 1: ["本机喇叭", "Speaker"], 2: ["蓝牙耳机", "Headset"], 3: ["当蓝牙音箱", "As speaker"], 5: ["键盘与语音", "Keys & voice"] };
function pickL(pair) {
  return L(pair[0], pair[1]);
}
function v2SettingsHome() {
  const rows = [
    ["talk", "wave", L("助手", "Assistant"), v2Who()],
    ["net", "net", L("网络", "Network"), state.link === "4G" ? `4G · ${state.sim === "int" ? L("内置卡", "built-in SIM") : L("外置卡", "external SIM")}` : "Wi-Fi · Metalio-Home"],
    ["screen", "sun", L("显示", "Display"), `${pickL(LIGHT[state.light])} · ${pickL(REFRESH[state.refresh])}${L("刷新", "")}`],
    ["read", "book", L("阅读与字库", "Reading & fonts"), `${state.fontName} ${state.fontSize} · ${L("扩展字库", "extended fonts ")}${state.fontpack ? L("已装", "on") : L("未装", "missing")}`],
    ["sound", "speaker", L("声音与蓝牙", "Sound & Bluetooth"), `${pickL(BT[state.btMode])} · ${state.tts ? L("回答出声", "spoken replies") : L("回答只看字", "text only")}`],
    ["device", "device", L("设备", "Device"), `${L("电量", "Battery")} ${state.battery}% · ${prototypeVersionLabel()}`],
  ];
  return `<section class="v-quick">${v2Level("sun", L("前光", "Light"), state.brightness, "bright")}${v2Level("speaker", L("音量", "Volume"), state.volume, "vol")}</section>
    <nav class="v-rows fill">${rows.map((r) => v2Row(...r)).join("")}</nav>`;
}
const assistantTalks = {};
// 当前助理会什么。提问、阅读上下文都有；通话要看它那边配没配实时语音。
function assistantCaps() {
  const current = state.assistants.find((item) => item.id === state.activeAssistant);
  return { ask: true, live: true, ctx: true, ...(current?.caps || {}) };
}
function removeAssistant(id) {
  state.assistants = state.assistants.filter((item) => item.id !== id);
  delete assistantTalks[id];
  if (state.activeAssistant === id) {
    if (state.assistants.length) useAssistant(state.assistants[0].id);
    else { stopSession(); state.activeAssistant = null; state.paired = false; state.pair = ""; v2.solo = blankTalk(); v2.notes = {}; }
  }
  state.assistantDetail = null;
  state.assistantUnpair = false;
}
function useAssistant(id) {
  const assistant = state.assistants.find((item) => item.id === id);
  if (!assistant) return;
  if (state.activeAssistant !== id) {
    stopSession();
    assistantTalks[state.activeAssistant] = { solo: v2.solo, notes: v2.notes };
    const saved = assistantTalks[id] || { solo: blankTalk(), notes: {} };
    v2.solo = saved.solo;
    v2.notes = saved.notes;
    state.q = "";
    state.a = [];
    state.answered = false;
    state.answerPage = 0;
  }
  state.activeAssistant = id;
  state.gatewayKind = assistant.backend;
  state.gateway = assistant.url;
  state.pair = assistant.code;
  state.paired = true;
  state.pairStatus = "";
}

function assistantConnection() {
  const detail = state.assistants.find((item) => item.id === state.assistantDetail);
  if (detail) {
    if (state.assistantUnpair) return `<div class="v-pairing"><h2>${L("移除助理？", "Remove assistant?")}</h2><p class="v-tip">${esc(detail.name)} · ${esc(detail.backend)}</p><p class="v-tip">${L("仅移除此设备保存的连接。其他助理和书籍保留；服务端授权需在原助理中撤销。", "Remove this saved connection. Other assistants and books remain; revoke server access separately.")}</p><button class="v-line" data-remove-assistant="${esc(detail.id)}">${L("确认移除", "Remove")}</button><button class="v-line" data-assistant-action="cancel-remove">${L("取消", "Cancel")}</button></div>`;
    return `<div class="v-pairing"><button class="v-line" data-assistant-action="list">${L("返回助理列表", "Back to assistants")}</button><h2>${esc(detail.name)}</h2><p class="v-tip">${esc(detail.backend)} · ${detail.id === state.activeAssistant ? L("当前使用", "In use") : L("已保存", "Saved")}</p><p class="v-tip">${detail.caps?.live === false ? L("支持提问和阅读上下文。通话还没配置：要助理那边先配好实时语音，再在配置页重新测试。", "Ask and reading context work. Call is not set up: the assistant needs a realtime voice service, then test again from the setup page.") : L("支持语音提问、Live 通话和阅读上下文。", "Voice questions, Live calls and reading context.")}</p><code class="v-assistant-url">${esc(detail.url)}</code>${detail.id === state.activeAssistant ? `<button class="v-line" data-pair-check>${L("检查连接（模拟）", "Check connection (demo)")}</button><p class="v-tip">${esc(state.pairStatus)}</p>` : `<button class="v-solid" data-use-assistant="${esc(detail.id)}">${L("使用这个助理", "Use this assistant")}</button>`}<button class="v-line" data-assistant-action="confirm-unpair">${L("移除此助理", "Remove assistant")}</button></div>`;
  }
  if (state.assistantUnpair) return `<div class="v-setup"><h2>${L("解除配对？", "Unpair assistant?")}</h2><p>${L("此设备将不再连接当前助理，书籍和阅读进度会保留。", "Disconnect this device. Books and reading progress are kept.")}</p><button class="v-line" data-assistant-action="unpair">${L("解除配对", "Unpair")}</button><button class="v-line" data-assistant-action="cancel">${L("取消", "Cancel")}</button></div>`;
  if (state.assistantSetup) return assistantSetupScreen();
  return `<div class="v-pairing"><div class="v-assistant-heading"><h2>${L("我的助理", "My assistants")} · ${state.assistants.length}</h2><button class="v-line" data-assistant-action="start">＋ ${L("添加助理", "Add assistant")}</button></div><p class="v-tip">${L("点「使用」切换。提问和通话都会使用选中的助理，各自的对话记录分开保存。", "Choose Use to switch both Ask and Call. Conversation histories stay separate.")}</p>${state.assistants.map((item) => `<section class="v-assistant-item ${item.id === state.activeAssistant ? "selected" : ""}"><div><b>${esc(item.name)}</b><span>${esc(item.backend)} · ${item.id === state.activeAssistant ? L("当前使用", "In use") : L("已保存", "Saved")}${item.caps?.live === false ? ` · ${L("通话未配置", "Call not set up")}` : ""}</span></div><div class="v-two">${item.id === state.activeAssistant ? `<span class="v-assistant-current">✓ ${L("正在使用", "In use")}</span>` : `<button class="v-line" data-use-assistant="${esc(item.id)}">${L("使用", "Use")}</button>`}<button class="v-line" data-detail-assistant="${esc(item.id)}">${L("管理", "Manage")}</button></div></section>`).join("")}${state.assistants.length ? "" : `<p class="v-tip">${L("还没有助理。点击「添加助理」，用手机或电脑完成连接。", "No assistants yet. Add one using your phone or computer.")}</p>`}</div>`;
}

// 设备上的「添加助理」。配置页在手机或电脑上做事，设备屏幕只显示地址、三步进度和结果。
function assistantSetupScreen() {
  const phase = state.setupPhase;
  const network = state.link === "wifi" || (state.localServiceMode === "ap" && state.localService);
  if (!network) return `<div class="v-setup"><h2>${L("先连接同一个网络", "Join the same network")}</h2><p>${L("手机或电脑与设备连接同一个 Wi-Fi，再打开配置页。4G 没有局域网地址。", "Put your phone or computer and this device on the same Wi-Fi, then open the setup page. Cellular has no local address.")}</p><button class="v-solid" data-go="wifi">${L("连接 Wi-Fi", "Connect Wi-Fi")}</button><button class="v-line" data-assistant-action="hotspot">${L("改用临时热点", "Use temporary hotspot")}</button><button class="v-line" data-assistant-action="cancel">${L("取消", "Cancel")}</button></div>`;
  if (phase === "done" && state.setupResult) {
    const r = state.setupResult;
    const row = (ok, name, notSet = false) => `<li class="is-${ok ? "done" : notSet ? "warn" : "bad"}"><i>${ok ? ic("check") : notSet ? "!" : ic("x")}</i><b>${name}</b></li>`;
    return `<div class="v-pairing"><h2>${L("连接好了", "Connected")}</h2><div class="v-card">${ic("check")}<span><b>${esc(r.name)}</b><em>${esc(r.backend)} · ${L("正在使用", "in use")}</em></span></div>
      <ol class="v-flow">${row(true, L("提问", "Ask"))}${row(r.caps.live, r.caps.live ? L("通话", "Call") : L("通话 · 未配置", "Call · not set up"), true)}${row(true, L("阅读上下文", "Reading context"))}</ol>
      <p class="v-tip">${r.caps.live ? L("提问和通话都走这个助理。", "Ask and Call both go to this assistant.") : L("通话要助理那边先配好实时语音，在配置页里重新测试。", "Call needs a realtime voice service on the assistant. Test again from the setup page.")}</p>
      <button class="v-solid" data-assistant-action="finish">${L("完成", "Done")}</button></div>`;
  }
  const hotspot = state.localServiceMode === "ap";
  const at = phase === "waiting" ? 0 : phase === "opened" ? 1 : 2;
  const mark = (i) => (phase === "done" ? "done" : phase === "failed" && i === 2 ? "bad" : i < at ? "done" : i === at ? "cur" : "todo");
  const steps = [L("打开配置页", "Open the page"), L("发给助理，粘贴回复", "Send to assistant, paste reply"), L("保存并连接测试", "Save, connect, test")];
  const flow = steps.map((name, i) => { const m = mark(i); return `<li class="is-${m}"><i>${m === "done" ? ic("check") : m === "bad" ? ic("x") : i + 1}</i><b>${name}</b></li>`; }).join("");
  const tip = phase === "failed" ? `${L("没连上：", "Could not connect: ")}${esc(state.setupNote)}`
    : phase === "connecting" ? `${L("设备正在测试：", "Testing now: ")}${esc(state.setupNote)}`
      : phase === "opened" ? L("配置页已打开。在页面里粘贴助理的回复。", "The page is open. Paste the assistant's reply there.")
        : L("还没人打开配置页。这个界面开着时，配置页才可用。", "Nobody has opened the page yet. It only works while this screen is open.");
  const access = hotspot ? L(`先连热点 ${state.localServiceSsid}`, `Join hotspot ${state.localServiceSsid}`) : L("手机或电脑连同一个 Wi-Fi", "Phone or computer on the same Wi-Fi");
  return `<div class="v-pairing"><h2>${L("在手机或电脑上连接", "Connect on a phone or computer")}</h2>
    <div class="v-pair-top">${SetupPage.qr(SetupPage.deviceUrl(), 76)}<div class="v-pair-address"><small>${L("扫码，或在浏览器打开", "Scan, or open in a browser")}</small><code>${esc(SetupPage.deviceUrl())}</code><small>${access}</small></div></div>
    <ol class="v-flow">${flow}</ol><p class="v-tip">${tip}</p>
    ${phase === "failed" ? `<button class="v-solid" data-assistant-action="retry">${L("重试", "Try again")}</button>` : `<button class="v-line" data-assistant-action="preview">${SetupPage.isOpen() ? L("配置页已打开（模拟）", "Page is open (simulated)") : L("在电脑上打开配置页（模拟）", "Open the page on a computer (simulated)")}</button>`}
    <button class="v-line" data-assistant-action="cancel">${L("取消", "Cancel")}</button></div>`;
}

function v2SettingsPane(id) {
  if (id === "talk") {
    const agent = state.pendingProvider === "agent";
    const ok = agentReady();
    const mine = agent ? assistantConnection()
      : `<p class="v-tip">${L(`${PROVIDERS[state.pendingProvider]}用的是随机送的订阅。只有按住说话，没有通话，也拿不到正在读的那一页。`, "Official plan: hold to talk only.")}</p>`;
    // 添加助理时整屏只做这一件事，不再同时放「谁来回答」。
    if (agent && state.assistantSetup) return mine;
    const reboot = state.pendingProvider !== state.provider
      ? `<button class="v-solid" data-reboot>${L("保存并重启", "Save and restart")}</button><p class="v-tip">${L("换助手要重启一次，读到哪一页会记住。", "Switching restarts the device.")}</p>` : "";
    return `<div class="v-card${agent && !ok ? " bad" : ""}">${ic(agent && !ok ? "x" : "check")}<span><b>${state.provider === "agent" ? v2Who() : PROVIDERS[state.provider]}</b>
        <em>${state.provider !== "agent" ? L("官方订阅，正在用", "Official plan, in use") : state.assistantSetup ? L("正在配置 · 原有配对保留", "Setup in progress") : ok ? L(`已配对 · 走 ${state.link === "4G" ? "4G" : "Wi-Fi"}`, "Paired") : L("尚未连接", "Not connected")}</em></span></div>
      ${v2Fld(L("谁来回答", "Who answers"), v2Seg(providerChoices(), state.pendingProvider, "data-provider"))}${mine}${reboot}`;
  }
  if (id === "net") {
    const sim = state.link === "4G" ? v2Fld(L("用哪张卡", "SIM"), v2Seg([["int", L("内置卡", "Built-in")], ["ext", L("外置卡", "External")]], state.sim, "data-sim")) : "";
    return `${v2Fld(L("怎么上网", "Connection"), v2Seg([["4G", "4G"], ["wifi", "Wi-Fi"]], state.link, "data-link"), L("换上网方式或换卡，设备会重启一次。", "Switching restarts the device."))}${sim}
      <nav class="v-rows">${`<button class="v-row" data-go="wifi"><span><b>Wi-Fi</b><em>${state.link === "wifi" ? "Metalio-Home · " + L("已连接", "connected") : L("没在用。点这里选网络", "Not in use. Tap to choose")}</em></span>${ic("go", "chev")}</button>`}</nav>
      ${v2Fld(L("不用网时，多久断开", "Disconnect when idle after"), v2Seg([["30", L("30 秒", "30 s")], ["60", L("1 分钟", "1 min")], ["120", L("2 分钟", "2 min")], ["0", L("不断", "Never")]], state.netGrace, "data-grace"), L("断得快更省电，下一次提问要多等一两秒连网。", "Sooner saves battery; the next question waits to reconnect."))}`;
  }
  if (id === "screen") {
    const hint = { clear: L("细节最清楚，残影最少，翻页慢一点。", "Sharpest, slowest."), balanced: L("画质和速度各让一步。", "A balance of both."), fast: L("翻页最快，残影会多一些。", "Fastest, more ghosting.") }[state.refresh];
    const themes = [["slash", L("斜切", "Slash")], ["white", L("白底", "White")], ["gray", L("网点", "Dots")], ["border", L("描边", "Outline")]];
    return `${v2Fld(L("前光", "Front light"), v2Seg(Object.entries(LIGHT).map(([k, v]) => [k, pickL(v)]), state.light, "data-light") + v2Level("sun", L("亮度", "Level"), state.brightness, "bright"))}
      ${calibrationControls()}
      ${v2Fld(L("刷新", "Refresh"), v2Seg(Object.entries(REFRESH).map(([k, v]) => [k, pickL(v)]), state.refresh, "data-refresh"), hint)}
      ${v2Fld(L("待机时屏上放什么", "Standby shows"), v2Seg([["classic", L("时钟和待办", "Clock & tasks")], ["wallpaper", L("壁纸", "Wallpaper")]], state.standbyStyle, "data-standby"))}
      ${v2Fld(L("应用页样式", "App grid style"), v2Seg(themes, state.theme === "pixel" ? "slash" : state.theme, "data-theme"))}`;
  }
  if (id === "read") {
    const shakeOptions = shakeSettings(v2Seg, v2Fld);
    return `<div class="v-card${state.fontpack ? "" : " bad"}">${ic(state.fontpack ? "check" : "x")}<span><b>${L("扩展字库", "Extended fonts")} · ${state.fontpack ? L("已安装", "installed") : L("未安装", "not installed")}</b>
        <em>${state.fontpack ? L("十万多个汉字，生僻字能显示", "100k+ characters available") : L("机内字库没有的字会画成方框", "Missing characters show as boxes")}</em></span></div>
      <p class="v-tip">${L("这不是屏的限制，是缺字形。把 extended-fonts.fontpack（约 255 MB）传到 SD 卡，再重新打开书。", "Not a screen limit. Copy extended-fonts.fontpack (about 255 MB) to the SD card.")}</p>
      <div class="v-two"><button class="v-line" data-go="transfer">${L("去传输页上传", "Open Transfer")}</button><button class="v-line dashed" data-fontpack>${state.fontpack ? L("假的：当作没装", "Fake: remove") : L("假的：当作已装好", "Fake: installed")}</button></div>
      ${v2Fld(L("正文字号", "Text size"), `<div class="v-step"><button data-size="-">A−</button><b>${state.fontSize}</b><button data-size="+">A+</button></div>`, L("字体、边距、行距在正文里点屏幕中间改。", "Font, margins and spacing are in the reading card."))}
      ${shakeOptions}
      <nav class="v-rows"><button class="v-row" data-go="pager"><span><b>${L("翻页器", "Page turner")}</b><em>${state.pagerOn ? "PageTurner-A · " + L("已连接", "connected") : L("没连。正文里加 / 减音量键也能翻页", "Not connected. Volume keys also turn pages")}</em></span>${ic("go", "chev")}</button></nav>`;
  }
  if (id === "sound") {
    const desc = { 1: L("用机身上的麦克风和喇叭。", "Built-in mic and speaker."), 2: L("声音从蓝牙耳机或音箱出来，通话更清楚。", "Audio goes to a Bluetooth headset."), 3: L("把手机的声音放到这台机器上。", "Play phone audio here."), 5: L("把按键和语音发给连着的电脑或手机。", "Send keys and voice to a paired device.") }[state.btMode];
    const hapt = state.haptic === "off" ? "off" : state.hapticLevel;
    return `${v2Fld(L("音量", "Volume"), v2Level("speaker", "", state.volume, "vol"))}
      ${v2Fld(L("助手回答时", "When the assistant answers"), v2Seg([["on", L("念出来", "Speak")], ["off", L("只看字", "Text only")]], state.tts ? "on" : "off", "data-tts"))}
      ${v2Fld(L("声音从哪里出", "Audio route"), v2Seg(Object.entries(BT).map(([k, v]) => [k, pickL(v)]), state.btMode, "data-btmode").replace("v-seg", "v-seg two"), `${desc} ${L("蓝牙名称", "Name")}：${esc(state.btName)}`)}
      ${v2Fld(L("按键震动", "Key haptics"), v2Seg([["off", L("关", "Off")], ["low", L("弱", "Low")], ["mid", L("中", "Mid")], ["high", L("强", "High")]], hapt, "data-hapt"))}`;
  }
  if (id === "device") {
    return `<div class="v-card">${ic("device")}<span><b>Metalio E-Ink4 Plus</b><em>${L("电量", "Battery")} ${state.battery}% · SD 12.4 / 29.1 GB</em></span></div>
      <nav class="v-rows">
        ${v2Row("power", "", L("电池与省电", "Battery & power"), L("5 分钟没操作待机，30 分钟关机", "Standby after 5 min, off after 30"))}
        ${v2Row("storage", "", L("存储与 U 盘", "Storage & USB"), state.usb ? L("电脑正在当 U 盘用", "Mounted on a computer") : L("SD 卡已插入", "SD card inserted"))}
        ${v2Row("lang", "", L("语言", "Language"), state.lang === "zh" ? "简体中文" : "English")}
        ${v2Row("diag", "", L("诊断", "Diagnostics"), L("日志、测试", "Logs, tests"))}
        ${v2Row("about", "", L("关于", "About"), prototypeVersionLabel())}
      </nav>`;
  }
  if (id === "power") {
    return `<div class="v-card">${ic("device")}<span><b>${L("电量", "Battery")} ${state.battery}%</b><em>${state.charging ? L("正在充电", "Charging") : L("没在充电", "Not charging")}</em></span></div>
      ${v2Fld(L("没操作的时候", "When idle"), v2Seg([["160", L("最省电", "Saver")], ["240", L("均衡", "Balanced")], ["320", L("最流畅", "Smooth")]], state.idleMhz, "data-mhz"), L("越省电，下一次翻页和唤醒越慢一点。", "More saving means a slower wake."))}
      <div class="v-rows">
        <div class="v-row"><span><b>${L("5 分钟没操作 → 待机", "5 min idle → standby")}</b><em>${L("屏上留着时钟和待办，按电源键醒来", "Clock stays on screen; power key wakes")}</em></span></div>
        <div class="v-row"><span><b>${L("待机满 30 分钟 → 关机", "30 min in standby → off")}</b><em>${L("开机后回到上次读的那一页", "Reopens the page you were reading")}</em></span></div>
      </div>
      ${v2Fld(L("充电电流", "Charging current"), v2Seg([["500", "0.5 A"], ["1000", "1 A"], ["1500", "1.5 A"]], state.chargeMa, "data-charge"), L("充电头功率小就选 0.5 A。", "Use 0.5 A with a weak charger."))}`;
  }
  // 设备下面的几页沿用官方的选项，只换外观。
  const pane = (tab) => {
    const keep = state.settingsTab;
    state.settingsTab = tab;
    const html = settingsPane();
    state.settingsTab = keep;
    return html;
  };
  if (id === "diag") return `<div class="v-old">${pane("log")}<h3>${L("测试", "Tests")}</h3><div class="v-grid2">${pane("test")}</div></div>`;
  return `<div class="v-old">${pane(id)}</div>`;
}
function v2Settings() {
  const id = state.settingsScreen;
  const titles = {
    list: L("设置", "Settings"), talk: L("助手", "Assistant"), net: L("网络", "Network"), screen: L("显示", "Display"),
    read: L("阅读与字库", "Reading & fonts"), sound: L("声音与蓝牙", "Sound & Bluetooth"), device: L("设备", "Device"),
    power: L("电池与省电", "Battery & power"), storage: L("存储与 U 盘", "Storage & USB"), lang: L("语言", "Language"),
    diag: L("诊断", "Diagnostics"), about: L("关于", "About"),
  };
  const body = id === "list" ? v2SettingsHome() : v2SettingsPane(id);
  return `${bar()}<main class="v-page v-set">${v2Head(titles[id] || titles.list)}<div class="v-pane">${body}</div></main>`;
}

// 待机：官方的斜切大时钟，下面是下一条待办和一句提示。
function v2Standby() {
  if (state.standbyStyle === "wallpaper") return standbyScreen();
  const todo = state.todos.find((x) => !x.done);
  return `<div class="v-standby">
    <div class="hero"><span class="hh">10</span><i class="slash"></i><span class="mm">48</span>
      <div class="date"><i></i>2026.11.17${state.lang === "zh" ? "<b>十月初八</b>" : ""}<span>${t("tue")}</span></div></div>
    <div class="v-sb-line"><b>${t("weather")} ${t("temp")}</b><span>${L("电量", "Battery")} ${state.battery}%</span></div>
    <div class="v-sb-todo"><b>${t("standbyTodo")}</b><span>${todo ? esc(todo.text) : t("noTodo")}</span></div>
    <p class="v-sb-hint">${ic("mic")}${L("长按 AI 唤醒键，醒来直接提问", "Hold the AI key to wake and ask")}</p></div>`;
}

// 其余应用沿用官方的结构，加上统一的页头。
function v2Screen() {
  const s = state.screen;
  if (s === "home") return v2Home();
  if (s === "ai") return v2Ai();
  if (s === "reader") return v2Reader();
  if (s === "settings") return v2Settings();
  const html = { detail, wallpaper, wall, tasks, transfer, store, shelf, book, buddy, wifi, pager }[s]();
  return `${bar()}<main class="v-page">${v2Head(names()[s])}<div class="v-app">${html.replace(/^<div class="bar">[\s\S]*?<\/div>/, "")}</div></main>`;
}

panel.addEventListener("click", (e) => {
  if (!isV2() || blocked()) return;
  const hit = (sel) => e.target.closest(sel);
  let el;
  if ((el = hit("[data-use-assistant]"))) {
    useAssistant(el.dataset.useAssistant);
    return render();
  }
  if ((el = hit("[data-detail-assistant]"))) {
    state.assistantDetail = el.dataset.detailAssistant;
    state.assistantUnpair = false;
    return render();
  }
  if ((el = hit("[data-remove-assistant]"))) {
    removeAssistant(el.dataset.removeAssistant);
    return render();
  }
  if ((el = hit("[data-assistant-action]"))) {
    const action = el.dataset.assistantAction;
    if (action === "start") {
      SetupPage.reset();
      state.assistantDetail = null;
      state.assistantSetup = true;
      state.assistantAdvanced = false;
      state.localService = state.link === "wifi";
      state.localServiceMode = "wifi";
      state.localServiceUrl = "http://192.168.1.86";
    }
    if (action === "hotspot") {
      state.localService = true;
      state.localServiceMode = "ap";
      state.localServiceUrl = "http://192.168.4.1";
    }
    if (action === "cancel" || action === "finish") {
      SetupPage.reset();
      state.assistantSetup = false;
      state.assistantUnpair = false;
      state.localService = false;
    }
    if (action === "retry") state.setupPhase = SetupPage.isOpen() ? "opened" : "waiting";
    if (action === "confirm-unpair") state.assistantUnpair = true;
    if (action === "cancel-remove") state.assistantUnpair = false;
    if (action === "list") { state.assistantDetail = null; state.assistantUnpair = false; }
    if (action === "unpair") {
      state.paired = false;
      state.pair = "";
      state.assistantUnpair = false;
      state.pairStatus = "";
    }
    if (action === "advanced") state.assistantAdvanced = !state.assistantAdvanced;
    if (action === "preview") { SetupPage.open(); return render(); }
    return render();
  }
  if (hit("[data-back]")) return back();
  if ((el = hit("[data-setting]"))) return openSetting(el.dataset.setting);
  if (hit("[data-ask]")) return v2Ask();
  if (hit("[data-live]")) return v2GoLive();
  if ((el = hit("[data-subject]"))) return v2SwitchSubject(el.dataset.subject);
  if ((el = hit("[data-homepage]"))) return void v2Chin(Number(el.dataset.homepage) ? 1 : -1);
  if ((el = hit("[data-sug]"))) {
    v2.pending = Number(el.dataset.sug);
    startListening();
    return stopListeningAndSend();
  }
  if ((el = hit("[data-answer]"))) {
    const n = state.answerPage + Number(el.dataset.answer);
    if (n < 0 || n >= nowTalk().a.length) return;
    state.answerPage = n;
    flash();
    return render();
  }
  if (hit("[data-new]")) {
    const live = state.live;
    stopSession();
    Object.assign(nowTalk(), blankTalk());
    v2Load();
    if (live) startLive();
    log(onBook() ? `第 ${state.fromPage + 1} 页的注清掉了，重新问。` : "随口问重新开始。");
    return render();
  }
  if (hit("[data-interrupt]")) return interruptLive();
  if (hit("[data-quote]")) return state.aiBack === "reader" ? back() : go("reader");
  if (hit("[data-v2photo]")) {
    state.photo = !state.photo;
    log(state.photo ? "拍好一张。下一句连照片一起发。" : "照片不要了。");
    return render();
  }
  if (hit("[data-fontfix]")) {
    state.sheet = true;
    state.sheetTab = 1;
    log("点了页脚的缺字提示：打开阅读卡片的字体页。");
    return render();
  }
  if (hit("[data-fontpack]")) {
    state.fontpack = !state.fontpack;
    log(state.fontpack ? "假的：当作扩展字库已经在 SD 卡上。正文里的方框换成了字。" : "假的：当作没装扩展字库。");
    return render();
  }
  if ((el = hit("[data-level]"))) {
    const [key, n] = el.dataset.level.split(":");
    const value = Number(n) * 10;
    if (key === "bright") {
      state.brightness = state.brightness === value ? value - 10 : value;
      if (state.brightness > 0 && state.light === "off") state.light = "both";
    } else {
      state.volume = state.volume === value ? value - 10 : value;
    }
    return render();
  }
  if ((el = hit("[data-gwkind]"))) {
    state.gatewayKind = el.dataset.gwkind;
    state.pairStatus = "";
    return render();
  }
  if (hit("[data-pair-check]")) {
    state.pairStatus = agentReady()
      ? L(`假的：${state.gatewayKind} 应答了，配对码对得上。`, "Fake: gateway answered, code accepted.")
      : L("地址要以 ws:// 或 wss:// 开头，配对码不能空。", "Address must start with ws:// or wss://.");
    log(`测试连接：${state.pairStatus}`);
    return render();
  }
  if ((el = hit("[data-hapt]"))) {
    state.haptic = el.dataset.hapt === "off" ? "off" : "on";
    if (state.haptic === "on") state.hapticLevel = el.dataset.hapt;
    return render();
  }
});
