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
  proxy: '<path d="M12 2.6 19 5.1c.6.2 1 .8 1 1.4v4.9c0 4.5-3.2 8-8 9.9-4.8-1.9-8-5.4-8-9.9V6.5c0-.6.4-1.2 1-1.4z"/><path d="M15.4 9.6H8.6m1.9-1.9L8.6 9.6l1.9 1.9M8.6 14.3h6.8m-1.9-1.9 1.9 1.9-1.9 1.9"/>',
  // Wi-Fi：自己画的，比例照 iPhone 量：同一圆心、±45°；沿中轴 扇形 4.7 / 空 1.67 / 弧带 3.68 / 空 1.67 / 弧带 3.68。
  // 每块先往里缩一圈、再用同宽的圆角描边撑回来，角就是圆的（扇形 0.8，弧带 0.55）
  wifi: '<path class="solid" fill="currentColor" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" d="M10.65 14.27L8.52 12.14A3.9 3.9 0 0 1 12.79 12.14Z"/><path class="solid" fill="currentColor" stroke="currentColor" stroke-width="1.1" stroke-linejoin="round" d="M4.34 8.3A9.5 9.5 0 0 1 16.97 8.3L15.14 10.13A6.92 6.92 0 0 0 6.17 10.13Z"/><path class="solid" fill="currentColor" stroke="currentColor" stroke-width="1.1" stroke-linejoin="round" d="M0.55 4.52A14.85 14.85 0 0 1 20.76 4.52L18.93 6.34A12.27 12.27 0 0 0 2.38 6.34Z"/>',
  // 4G 信号：自己画的，比例照 iPhone 量：四根实心圆角柱，柱宽比空隙宽两成，高 36% / 54% / 75% / 100%
  cell: '<g fill="currentColor" stroke="none"><rect x="0.00" y="7.7" width="3.0" height="4.3" rx="1.1"/><rect x="5.53" y="5.5" width="3.0" height="6.5" rx="1.1"/><rect x="11.06" y="3" width="3.0" height="9.0" rx="1.1"/><rect x="16.59" y="0" width="3.0" height="12" rx="1.1"/></g>',
  thermo: '<path d="M10 4.5a2 2 0 0 1 4 0v9.3a4 4 0 1 1-4 0z"/><path d="M12 9.5v6.2"/>',
  plane: '<path d="M10.5 3.5h1.8l3.4 7h4.1a1.7 1.7 0 0 1 0 3.4h-4.1l-3.4 7h-1.8l1.6-7H8.3l-1.6 2.2H5l1-3.9-1-3.9h1.7l1.6 2.2h3.8z"/>',
  mute: '<path d="M4 9.5v5h3.5l5 4v-13l-5 4zM16.5 9.5l5 5M21.5 9.5l-5 5"/>',
  x: '<path d="M6 6l12 12M18 6L6 18"/>',
  pause: '<path d="M8.5 5v14M15.5 5v14"/>',
  stop: '<rect x="5" y="5" width="14" height="14" rx="2.5" fill="currentColor" stroke="none"/>',
  cam: '<path d="M3.5 8h3.2l1.6-2.5h7.4L17.3 8h3.2v11h-17z"/><circle cx="12" cy="13.2" r="3.2"/>',
  speaker: '<path d="M4 9.5v5h3.5l5 4v-13l-5 4zM16 9a4.2 4.2 0 0 1 0 6M18.6 6.4a7.8 7.8 0 0 1 0 11.2"/>',
  net: '<path d="M4.5 20v-3M9.5 20v-7M14.5 20V9M19.5 20V4"/>',
  sunOff: '<circle cx="12" cy="12" r="3.6"/><path d="M12 2.8v2.4M12 18.8v2.4M2.8 12h2.4M18.8 12h2.4M4 4l16 16"/>',
  sun: '<circle cx="12" cy="12" r="3.6"/><path d="M12 2.8v2.4M12 18.8v2.4M2.8 12h2.4M18.8 12h2.4M5.5 5.5l1.7 1.7M16.8 16.8l1.7 1.7M5.5 18.5l1.7-1.7M16.8 7.2l1.7-1.7"/>',
  device: '<rect x="6.5" y="2.5" width="11" height="19" rx="2"/><path d="M10 18h4"/>',
  shelf: '<path d="M5 4v15.5M9.5 4v15.5M13.5 5.5l4.5 14M3 20h18"/>',
  buddy: '<path d="M4 4.5h16v11.5h-9l-4.5 4v-4H4z"/><path d="M8.5 10.3h.01M12 10.3h.01M15.5 10.3h.01"/>',
  store: '<path d="M5 8.5h14l-1 12H6zM9 8.5V7a3 3 0 0 1 6 0v1.5"/>',
};
function ic(name, cls = "") {
  return `<svg class="ic${cls ? " " + cls : ""}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${V2_ICONS[name]}</svg>`;
}

// ── 状态栏（新设计）。左：网络，走代理时紧挨着一个盾牌；中：时间；右：静音、电池（数字反白写在电池里，不带 %）。
// 三组都按同一条中线摆，不靠字体基线。点左边或往下拉，出快捷面板。
function proxyInForce() {
  if (!state.proxyAddr || (proxyPasswordOnly() && !state.proxySsPass)) return false;
  return state.link === "wifi" ? state.proxyWifi : state.proxyCell;
}
// 状态栏图标：每个都裁到自己的轮廓（不是 24×24 的整格），再统一画成同样高、同样粗的线。
// 整格里留白各不相同（Wi-Fi 只占 5–20，盾牌占 3–21），直接放进同样大的框里就会一个高一个矮、一个上一个下。
const SB_BOX = { wifi: "0 0 21.31 15.07", proxy: "4 2.6 16 18.7", plane: "5 3.5 16.5 17.4", mute: "4 5.5 17.5 13", cell: "0 0 19.59 12" };
// 本机有 sim/local/icons.js（不进 git）就用里面的图标，同一比例画，各自高矮照原样；没有就用上面自己画的
const LOCAL_ICON = window.LOCAL_ICONS || {};
for (const [k, v] of Object.entries(LOCAL_ICON)) if (v.svg) { V2_ICONS[k] = v.svg; SB_BOX[k] = v.box; }
function sbIcon(name, cls = "sbi") {
  if (LOCAL_ICON[name]) return `<svg class="${cls} local" style="--h:${LOCAL_ICON[name].h}" viewBox="${SB_BOX[name]}" aria-hidden="true">${V2_ICONS[name]}</svg>`;
  return `<svg class="${cls}" viewBox="${SB_BOX[name]}" fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${V2_ICONS[name]}</svg>`;
}
function v2NetGlyph() {
  if (state.link === "4G") return `<i class="sb-sig" aria-label="4G">${sbIcon("cell")}</i>`;
  if (!state.wifiNearby && state.linkMode === "wifi") return `<i class="sb-plane" aria-label="${L("Wi-Fi 没连上", "Wi-Fi offline")}">${sbIcon("plane")}</i>`;
  // 省电睡下了（一用网就会连上）：扇形全灰，不画飞机。飞机只代表真的上不了网
  if (state.wifiAsleep) return `<i class="sb-wifi asleep" aria-label="${L("Wi-Fi 休眠中", "Wi-Fi asleep")}">${sbIcon("wifi")}</i>`;
  return `<i class="sb-wifi" aria-label="Wi-Fi">${sbIcon("wifi")}</i>`;
}
function v2Battery() {
  const n = Math.max(0, Math.min(100, state.battery));
  return v2BatteryOutlined(n);
  // 电量越少，底色越短；数字一直写在中间。墨水屏上黑底白字和白底黑字交界处用描边保证两边都读得出。
  // 充电时数字后面跟一道闪电，都在电池框里（照 iPhone）
  const bolt = state.charging ? `<svg class="bolt" viewBox="0 0 6.4 11" aria-hidden="true"><path d="M3.9 0 .2 6.2h2.6L2.3 11 6.2 4.6H3.6z"/></svg>` : "";
  return `<i class="sb-batt${state.charging ? " charging" : ""}" style="--lvl:${n}" aria-label="${L("电量", "Battery")} ${n}${state.charging ? L("，在充电", ", charging") : ""}"><b>${n}${bolt}</b></i>`;
}
// 电池：自己画的外框（比例照 iPhone 量：框厚 7、外圆角 20、内圆角 13，单位是 53 高的格子），
// 空腔正好写下三个字：「100」，或者「57」加闪电（满电不会在充电，所以不会出现「100」加闪电）。
// 里面一块中灰圆角块按电量从左往右长（满格先画好、右边按电量裁掉，左边圆角一直在），四周和外框留一圈纸色细缝；
// 黑色粗体数字在空腔正中，不带 %，充电时后面跟闪电
const BATT = { h: 53.12, t: 7, ro: 20, ri: 13, cavityW: 71, capGap: 5.7, capW: 7.3, capH: 20.5, gap: 3.5 };
function battOutline() {
  const { h, t, ro, ri, cavityW, capGap, capW, capH } = BATT;
  const w = cavityW + 2 * t, iw = cavityW, ih = h - 2 * t;
  const rr = (x, y, w2, h2, r) => `M${x + r} ${y}H${x + w2 - r}A${r} ${r} 0 0 1 ${x + w2} ${y + r}V${y + h2 - r}A${r} ${r} 0 0 1 ${x + w2 - r} ${y + h2}H${x + r}A${r} ${r} 0 0 1 ${x} ${y + h2 - r}V${y + r}A${r} ${r} 0 0 1 ${x + r} ${y}Z`;
  const cx = w + capGap, cy = (h - capH) / 2;
  const cap = `M${cx} ${cy}C${cx + capW * 0.66} ${cy + 0.1} ${cx + capW} ${cy + capH * 0.22} ${cx + capW} ${cy + capH / 2}S${cx + capW * 0.66} ${cy + capH - 0.1} ${cx} ${cy + capH}Z`;
  return { d: rr(0, 0, w, h, ro) + rr(t, t, iw, ih, ri) + cap, box: `0 0 ${cx + capW} ${h}`, cavity: [t, t, iw, ih], total: cx + capW };
}
function v2BatteryOutlined(n) {
  const o = battOutline(), [cx, cy, cw, ch] = o.cavity, gap = BATT.gap;
  const fx = cx + gap, fy = cy + gap, fw = cw - 2 * gap, fh = ch - 2 * gap, fr = BATT.ri - gap;
  const pct = (v, total) => `${(v / total) * 100}%`;
  const bolt = !state.charging ? "" : LOCAL_ICON.bolt
    ? `<svg class="bolt2" viewBox="${LOCAL_ICON.bolt.box}" aria-hidden="true"><path d="${LOCAL_ICON.bolt.d}"/></svg>`
    : `<svg class="bolt2" viewBox="0 0 6.4 11" aria-hidden="true"><path d="M3.9 0 .2 6.2h2.6L2.3 11 6.2 4.6H3.6z"/></svg>`;
  return `<i class="sb-batt2" aria-label="${L("电量", "Battery")} ${n}${state.charging ? L("，在充电", ", charging") : ""}">
    <svg viewBox="${o.box}" aria-hidden="true"><defs><clipPath id="batt-lvl"><rect x="${fx}" y="${fy}" width="${(fw * n) / 100}" height="${fh}"/></clipPath></defs>
      <path d="${o.d}" fill="currentColor" fill-rule="evenodd"/><rect class="lvl" x="${fx}" y="${fy}" width="${fw}" height="${fh}" rx="${fr}" clip-path="url(#batt-lvl)"/></svg>
    <b style="left:${pct(cx, o.total)};top:${pct(cy, BATT.h)};width:${pct(cw, o.total)};height:${pct(ch, BATT.h)}">${n}${bolt}</b></i>`;
}
function v2Bar(center) {
  const proxy = proxyInForce() ? `<i class="sb-proxy" aria-label="${L("在走代理", "Proxy in use")}">${sbIcon("proxy")}</i>` : "";
  const mute = state.volume === 0 ? `<i class="sb-mute">${sbIcon("mute")}</i>` : "";
  return `<div class="bar sb" data-quick-open><span class="sb-left">${v2NetGlyph()}${proxy}</span><span class="c">${center}</span><span class="right">${mute}${v2Battery()}</span></div>`;
}

// ── 快捷面板：从状态栏往下拉。上面四个开关（点一下切换，长按进设置），下面前光和音量。
function v2QuickPanel() {
  const net = state.link === "4G" ? "4G" : "Wi-Fi";
  const netSub = state.link === "4G" ? L("已连上", "Connected")
    : !state.wifiNearby && state.linkMode === "wifi" ? L("没连上", "Offline")
    : state.wifiAsleep ? L("休眠中 · Metalio-Home", "Asleep · Metalio-Home") : "Metalio-Home";
  const modeName = { auto: L("自动", "Auto"), "4G": "4G", wifi: "Wi-Fi" }[state.linkMode];
  const proxyOk = state.proxyAddr && (!proxyPasswordOnly() || state.proxySsPass);
  const proxyOn = state.link === "wifi" ? state.proxyWifi : state.proxyCell;
  const proxyKind = { http: "HTTP", socks5: "SOCKS5", ss: "SS", trojan: "Trojan" }[state.proxyKind];
  // 小字写用的哪种代理；开没开写在右上角，和网络那块的「自动」一个位置
  const proxySub = !proxyOk ? L("还没设置", "Not set up") : proxyKind;
  const proxyCorner = !proxyOk ? "" : proxyOn ? L("开启", "On") : L("关闭", "Off");
  // corner：右上角的小标（「自动」），不占标题和小字的位置
  // 图标和状态栏一样裁到轮廓：Wi-Fi、4G、盾牌一样高，不会一个大一个小
  const tile = (key, icon, title, sub, on, dim, corner = "") =>
    `<button class="q-tile${on ? " on" : ""}${dim ? " dim" : ""}" data-quick="${key}"><i class="q-ic">${sbIcon(icon, "qi")}</i><span><b>${title}</b><em>${sub}</em></span>${corner ? `<i class="q-corner">${corner}</i>` : ""}</button>`;
  // 一排：左边的图标就是开关（前光：开 / 关；音量：有声 / 静音），点格子调到那一档。
  // 关掉时格子全空、右边写「关」或「静音」，原来的档位记着，再点图标回到原来那一档。色温没有「关」，图标只是标签。
  const RK = 0.172;   // 本机图标：每单位多少 px（喇叭约 13px 高）
  const rowIc = ([local, fallback]) => {
    const v = LOCAL_ICON[local];
    if (!v || !v.row) return ic(fallback);
    return `<svg class="q-rowic" viewBox="${v.x} -106 ${v.w} 117" style="width:${(v.w * RK).toFixed(1)}px;height:${(117 * RK).toFixed(1)}px" fill="currentColor" aria-hidden="true">${v.row}</svg>`;
  };
  const level = (icon, label, value, key, off, offText, dim, steps = 10, valueText) => {
    const blocks = Array.from({ length: steps }, (_, i) => `<button data-level="${key}:${steps === 10 ? i + 1 : i + 1}" class="${!off && i < (steps === 10 ? Math.round(value / 10) : value) ? "on" : ""}" aria-label="${label} ${steps === 10 ? (i + 1) * 10 : i + 1}"></button>`).join("");
    const head = offText !== undefined
      ? `<button class="q-switch${off ? " off" : ""}" data-level-off="${key}" aria-label="${label}${off ? L("：已关，点一下打开", " off, tap to turn on") : L("：点一下关掉", ", tap to turn off")}">${rowIc(icon)}</button>`
      : `<span class="q-switch label">${rowIc(icon)}</span>`;
    return `<div class="q-level${dim ? " dim" : ""}">${head}<div class="v-blocks${steps !== 10 ? " s" + steps : ""}">${blocks}</div><b>${off ? offText : valueText ?? value}</b></div>`;
  };
  const lightOff = state.light === "off" || state.brightness === 0;
  // 只放前光和色温：音量有实体键，黑色深度是调一次就不动的设置，都不占这里。
  // 每排左边的图标跟着档位变：灯泡 关 / 暗 / 亮，温度计 冷 / 中 / 暖。
  // 本机有 sim/local 的图标就用它们（同一个比例、同一条基线），没有就用自己画的线条图标
  const bulb = lightOff ? "lightbulb.slash" : state.brightness <= 50 ? "lightbulb.min" : "lightbulb.max";
  const thermo = state.warmth < 40 ? "thermometer.snowflake" : state.warmth > 60 ? "thermometer.sun" : "thermometer.variable";
  return `<div class="q-mask" data-quick-close></div><section class="q-panel">
      <div class="q-grid">
        ${tile("link", state.link === "4G" ? "cell" : "wifi", net, netSub, true, false, state.linkMode === "auto" ? modeName : "")}
        ${tile("proxy", "proxy", L("代理", "Proxy"), proxySub, proxyOk && proxyOn, !proxyOk, proxyCorner)}
      </div>
      ${level([bulb, lightOff ? "sunOff" : "sun"], L("前光", "Light"), state.brightness, "bright", lightOff, L("关", "Off"))}
      ${level([thermo, "thermo"], L("色温", "Warmth"), state.warmth, "warm", false, undefined, lightOff)}
      <i class="q-grip"></i>
    </section>`;
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
// ── 悬窗：在正文页下方问这一页、聊这一页，书页不动，不用切出去。
// 回答太长、要看以前的问答、要随口问或拍照时点「全屏」去完整助手页，同一场对话；从那里返回又回到悬窗。
function v2OpenFloat(live) {
  // 官方云拿不到正在读的这一页，照旧进完整的百问页；还没连上自己的助手也去完整页（那里有「去连接」）
  if (state.provider !== "agent" || !agentReady()) return live ? v2GoLive() : go("ai");
  if (live && !assistantCaps().live) return toast(L("这个助理还没配置通话", "This assistant has no Call set up"));
  const was = state.float;
  state.sheet = false;
  state.float = true;
  if (!was) {
    v2FloatFollowPage();
    state.liveMuted = false;
    log(`在第 ${state.page + 1} 页下方打开悬窗${live ? "，接通" : ""}。书页不动。${state.answered ? "这一页问过，那条注接着显示。" : ""}`);
  }
  if (live && !state.live) startLive();
  render();
}
// 悬窗跟着正在读的这一页走：每一页的问答各自留着，通话不断，下一句带上新的这一页。
function v2FloatFollowPage() {
  if (state.sessionType === "book" && state.fromPage !== null) nowTalk().p = state.answerPage;
  state.aiBack = "reader";
  state.sessionType = "book";
  state.fromPage = state.page;
  v2Load();
}
function v2CloseFloat(why) {
  if (!state.float) return false;
  nowTalk().p = state.answerPage;
  stopSession();
  state.float = false;
  log(`${why}：收起悬窗，接着读第 ${state.page + 1} 页。${state.answered ? "这一页的注留着。" : ""}`);
  render();
  return true;
}
// 悬窗上沿落在两行之间：在 3/4 高附近找书页上一行的下沿，不让悬窗切过半行字。
function v2SnapFloat() {
  const float = document.querySelector(".v-float"), body = document.querySelector(".v-read .read-body");
  if (!float || !body) return;
  const read = float.parentElement.getBoundingClientRect();
  const want = read.top + read.height * 0.25;   // 3/4 高时的上沿
  let best = 0;
  const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT);
  const range = document.createRange();
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    range.selectNodeContents(node);
    for (const r of range.getClientRects()) if (r.bottom <= want + 6 && r.bottom > best) best = r.bottom;
  }
  // 只在 3/4 附近（上下两行以内）对齐；量不到或离谱就保留 CSS 的 75%
  // 屏幕整体是缩放画的：量到的是屏幕像素，样式高度要换回页面自己的像素
  const scale = read.height / float.parentElement.offsetHeight || 1;
  if (best && best > want - read.height * 0.12) float.style.height = `${Math.round((read.bottom - best) / scale - 8)}px`;   // 留出上沿的抖点阴影
  else float.style.height = "";
}
function v2SnapFloatSoon() {
  v2SnapFloat();
  requestAnimationFrame(v2SnapFloat);          // 排版落定后再量一次
  document.fonts?.ready.then(v2SnapFloat);     // 宋体加载完行高会变
}
function v2ExpandFloat() {
  nowTalk().p = state.answerPage;
  const live = state.live, muted = state.liveMuted;
  state.float = false;
  go("ai");                 // 同一场：这一页的注、正在进行的通话都带过去
  state.liveMuted = muted;
  state.floatExpanded = true;
  log(`全屏：换到完整的助手页。${live ? "通话没断。" : ""}返回会回到第 ${state.page + 1} 页的悬窗。`);
  render();
}
function v2FloatBody(s) {
  if (state.live) return v2LiveBody();
  if (state.listening) return `<div class="v-listen">${ic("mic")}<b>${L("在听", "Listening")}</b><span>${L("松开就发出去", "Release to send")}</span></div>`;
  if (s.a.length) {
    const long = s.a.length > 2
      ? `<button class="v-float-more" data-float-expand>${L("回答较长 · 全屏看全部", "Long answer · full screen")}${ic("go")}</button>` : "";
    return `${v2Answer(s)}${long}`;
  }
  const sugs = v2Sugs().slice(0, 2).map(([label], i) => `<button data-sug="${i}">${glyphs(esc(label))}${ic("go")}</button>`).join("");
  return `<p class="v-float-tip">${L("正文已经带上。按住下面说话，问完接着读。", "The page is attached. Hold to talk, then keep reading.")}</p><div class="v-float-sugs">${sugs}</div>`;
}
function v2Float() {
  const s = nowTalk();
  const pager = !state.live && s.a.length > 1 && !state.listening
    ? `<div class="v-pages"><button data-answer="-1" ${state.answerPage > 0 ? "" : "disabled"}>${ic("back")}</button><span>${state.speaking ? L("在说 · 短按 AI 唤醒键停下", "Speaking") : `${state.answerPage + 1} / ${s.a.length}`}</span><button data-answer="1" ${state.answerPage < s.a.length - 1 ? "" : "disabled"}>${ic("go")}</button></div>`
    : "";
  const foot = state.live
    ? `<div class="v-talk v-callbar">
      <button data-mute>${ic(state.liveMuted ? "micOff" : "mic")}${state.liveMuted ? L("取消静音", "Unmute") : L("静音", "Mute")}</button>
      <button data-interrupt>${ic("stop", "stop")}${L("打断", "Interrupt")}</button>
      <button class="tolive" data-mode="ask">${ic("x")}${L("挂断", "End")}</button></div>`
    : `<div class="v-talk${state.listening ? " rec" : ""}"><i></i><span data-hold>${state.listening ? L("松开发送", "Release to send") : L("按住说话", "Hold to talk")}</span>
      ${callEntry() ? `<button class="tolive" data-live>${ic("wave")}${L("通话", "Call")}</button>` : ""}</div>`;
  return `<div class="v-float">
    <div class="v-float-head"><b>${state.live ? L("聊这一页", "Talking about it") : L("问这一页", "Ask this page")}</b><span>${L(`第 ${state.page + 1} 页`, `p.${state.page + 1}`)} · ${esc(v2Who())}</span>
      <button class="v-txt" data-float-expand>${L("全屏", "Full screen")}</button><button class="v-float-x" data-float-close aria-label="${L("收起", "Close")}">${ic("x")}</button></div>
    <div class="v-float-body">${v2FloatBody(s)}</div>${pager}${foot}</div>`;
}
function v2Ask() {
  if (state.screen === "reader") return v2OpenFloat(false);
  if (state.screen !== "ai") return go("ai");
  if (state.live) switchMode("ask");
}
function v2GoLive() {
  if (state.screen === "reader" && state.provider === "agent" && agentReady()) return v2OpenFloat(true);
  if (state.screen !== "ai") go("ai");
  if (state.provider !== "agent") return toast(L("通话要用「自建」", "Calls need Custom"));
  if (!agentReady()) return render();
  if (!assistantCaps().live) return toast(L("这个助理还没配置通话", "This assistant has no Call set up"));
  if (!state.live) switchMode("live");
}
function openSetting(id) {
  state.proxyPicker = false; // 弹出列表不跨页留着，和设备一样
  if (state.screen === "settings") {
    state.settingsScreen = id;
    return render();
  }
  go("settings");
  state.settingsScreen = id;
  v2.direct = id;
  render();
}
const V2_PARENT = { power: "device", storage: "device", lang: "device", diag: "device", about: "device", proxy: "net", bt: "sound", pager: "read" };
// 返回 true = 这次返回已经在设置里往上走了一级。
function v2Back() {
  if (state.screen === "ai" && state.floatExpanded && stack[stack.length - 1] === "reader") {
    // 从悬窗点「全屏」出来的：回到书页，悬窗还在，通话不断
    v2LeaveAi();
    state.floatExpanded = false;
    state.screen = stack.pop();
    state.float = true;
    v2FloatFollowPage();
    log(`回到第 ${state.page + 1} 页，悬窗还在。${state.live ? "通话没断。" : ""}`);
    flash();
    render();
    return true;
  }
  if (state.screen !== "settings") return false;
  // 和设备一样：协议或加密方式的弹出列表开着时，返回键先关掉列表，还留在代理页。
  if (state.settingsScreen === "proxy" && state.proxyPicker) {
    state.proxyPicker = false;
    render();
    return true;
  }
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
  // 首页永远是两块斜切。有自己的助理：提问和通话。没有：通话那块换成「接着读」，
  // 下面原来放接着读的位置让给待办，一次看到三条。
  const call = callEntry();
  const pairArt = `<polygon class="dark" data-ask points="0.6,0.6 127,0.6 96,133.4 0.6,133.4" />
        <polygon ${call ? `class="${liveOk ? "" : "off"}" data-live` : `data-go="reader"`} points="134,0.6 221.4,0.6 221.4,133.4 103,133.4" />`;
  const callBlock = call
    ? `<div class="v-call${liveOk ? "" : " off"}">${ic("wave")}<b>${L("通话", "Call")}</b><span>${liveOk ? L("接通就聊", "Just talk") : L("还没配置通话", "Call not set up")}</span></div>`
    : `<div class="v-call v-read-block">${ic("book")}<i class="v-track"><i style="width:${pct}%"></i></i><b>${L("接着读", "Read")}</b><span>${L("三国演义 · 第", "Three Kingdoms · p.")} ${state.page + 1} ${L("页", "")}</span></div>`;
  const bookCard = `<button class="v-book" data-go="reader">
      <span class="v-cover">三国<br>演义</span>
      <span class="v-book-main"><b>三国演义</b><em>${L("第一回 · 第", "Ch. 1 · p.")} ${state.page + 1} / ${pages.length} ${L("页", "")}</em><i class="v-track"><i style="width:${pct}%"></i></i></span>
      <span class="v-wedge">${L("接着读", "Read")}${ic("go")}</span>
    </button>
    <button class="v-todo" data-go="tasks"><b>${open.length}</b><span>${open.length ? esc(open[0].text) : L("今天没有待办", "Nothing to do today")}</span>${open.length > 1 ? `<em>${L(`还有 ${open.length - 1} 条`, `+${open.length - 1}`)}</em>` : ""}${ic("go")}</button>`;
  const todoList = `<button class="v-todos" data-go="tasks">
      <span class="v-todos-head"><b>${L("今天的待办", "Today")}</b><em>${open.length ? L(`${open.length} 条没做`, `${open.length} open`) : ""}</em>${ic("go")}</span>
      ${open.length
        ? open.slice(0, 3).map((x) => `<span class="v-todos-item"><i></i><span>${esc(x.text)}</span></span>`).join("")
        : `<span class="v-todos-none">${L("今天没有待办", "Nothing to do today")}</span>`}
    </button>`;
  return `${bar()}<main class="v-home">
    <section class="v-hero">
      <div class="v-clock"><span>10</span><i></i><span>48</span></div>
      <div class="v-date"><span>2026.11.17</span><b>${L("十月初八", "November 17")}</b><span>${t("tue")} · ${t("weather")} ${t("temp")}</span></div>
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
    ${call ? bookCard : todoList}
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
    body = `<div class="v-setup"><h2>${L("还没连上你的助手", "Assistant not connected")}</h2><p>${L("用手机或电脑添加一个助理，提问和通话都走你自己的 OpenClaw 或 Hermes。", "Add one from your phone or computer. Ask and Call then go to your own OpenClaw or Hermes.")}</p>
      <button class="v-solid" data-setting="talk">${L("去连接", "Connect")}${ic("go")}</button></div>`;
  } else if (state.live) body = v2LiveBody();
  else if (state.listening) body = `<div class="v-listen">${ic("mic")}<b>${L("在听", "Listening")}</b><span>${L("松开就发出去", "Release to send")}</span></div>`;
  else if (s.a.length) body = v2Answer(s);
  else body = v2Welcome(book);

  let foot = "";
  if (ready && state.live) {
    // 通话里沿用提问页的胶囊条：左边「静音」「打断」，右边是和「通话」同位置、同斜切的黑色「挂断」（照真机）。
    foot = `<div class="v-talk v-callbar">
      <button data-mute>${ic(state.liveMuted ? "micOff" : "mic")}${state.liveMuted ? L("取消静音", "Unmute") : L("静音", "Mute")}</button>
      <button data-interrupt>${ic("stop", "stop")}${L("打断", "Interrupt")}</button>
      <button class="tolive" data-mode="ask">${ic("x")}${L("挂断", "End")}</button></div>`;
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
    ${state.sheet ? `<div class="v-shade" data-sheet-shade></div>` : ""}${sheet}${state.float ? v2Float() : ""}</div>`;
}

// ── 设置：按「我想做什么」分六项，每项直接写着现在的值。不滚动。
function v2Seg(items, current, attr) {
  return `<div class="v-seg">${items.map(([id, name]) => `<button ${attr}="${id}" class="${String(current) === String(id) ? "on" : ""}">${name}</button>`).join("")}</div>`;
}
function v2Fld(label, body, note = "") {
  return `<div class="v-fld"><label>${label}</label>${body}${note ? `<p>${note}</p>` : ""}</div>`;
}
function v2Level(icon, label, value, key) {
  // 前光关着时记住的亮度不算数：格子全空、写「关」，和下拉栏一致
  const off = key === "bright" && (state.light === "off" || value === 0);
  const n = off ? 0 : Math.round(value / 10);
  const blocks = Array.from({ length: 10 }, (_, i) => `<button data-level="${key}:${i + 1}" class="${i < n ? "on" : ""}" aria-label="${label} ${(i + 1) * 10}"></button>`).join("");
  return `<div class="v-level">${ic(icon)}<span>${label}</span><div class="v-blocks">${blocks}</div><b>${off ? L("关", "Off") : value}</b></div>`;
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
    ["net", "net", L("网络", "Network"), (state.linkMode === "auto" ? L("自动 · ", "Auto · ") : "") + (state.link === "4G" ? `4G · ${state.sim === "int" ? L("内置卡", "built-in SIM") : L("外置卡", "external SIM")}` : "Wi-Fi · Metalio-Home")],
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
    // 换了马上生效，不重启；正在进行的对话会结束
    const reboot = `<p class="v-tip">${L("换一个马上生效，不用重启。正在进行的对话会结束。", "Takes effect at once, no restart. A conversation in progress ends.")}</p>`;
    return `<div class="v-card${agent && !ok ? " bad" : ""}">${ic(agent && !ok ? "x" : "check")}<span><b>${state.provider === "agent" ? v2Who() : PROVIDERS[state.provider]}</b>
        <em>${state.provider !== "agent" ? L("官方订阅，正在用", "Official plan, in use") : state.assistantSetup ? L("正在配置 · 原有配对保留", "Setup in progress") : ok ? L(`已配对 · 走 ${state.link === "4G" ? "4G" : "Wi-Fi"}`, "Paired") : L("尚未连接", "Not connected")}</em></span></div>
      ${v2Fld(L("谁来回答", "Who answers"), v2Seg(providerChoices(), state.pendingProvider, "data-provider"))}${mine}${reboot}`;
  }
  if (id === "net") {
    const cell = state.link === "4G";
    const auto = state.linkMode === "auto";
    // 选了「自动」或「4G」都可能用到卡，所以两种都给选卡；只用 Wi-Fi 时不给。
    const sim = state.linkMode !== "wifi" ? v2Fld(L("用哪张卡", "SIM"), v2Seg([["int", L("内置卡", "Built-in")], ["ext", L("外置卡", "External")]], state.sim, "data-sim"), L("卡号（ICCID）", "Card number (ICCID)") + " 89860000000000000000") : "";
    // 最上面先说现在连得怎么样：进这一页多半就是来看这个的。强度用话说，数字放后面。
    // 自动时卡片上说明为什么用的是它；挂断后要换的也写在这里。
    const why = state.linkPending ? L(` · 挂断后换到 ${state.linkPending === "4G" ? "4G" : "Wi-Fi"}`, ` · switches to ${state.linkPending === "4G" ? "4G" : "Wi-Fi"} after the call`)
      : !auto ? "" : cell ? L(" · 附近没有存过的 Wi-Fi", " · no saved Wi-Fi nearby") : L(" · 自动选的", " · picked automatically");
    const lost = !cell && !state.wifiNearby;   // 只用 Wi-Fi，又不在它附近
    const status = lost
      ? `<div class="v-card bad">${ic("x")}<span><b>Wi-Fi · ${L("没连上", "not connected")}</b><em>${L("附近没有存过的网络。出门也要用网就选「自动」", "No saved network nearby. Choose Auto to use 4G away from it")}</em></span></div>`
      : `<div class="v-card">${ic("check")}<span><b>${cell ? "4G" : "Wi-Fi"} · ${L("信号好", "good signal")}</b><em>${cell ? L("已连上 · 强度 24 / 31", "Connected · strength 24 / 31") : "Metalio-Home · -42 dBm"}${why}</em></span></div>`;
    const modeNote = auto ? L("有存过的 Wi-Fi 就用 Wi-Fi，没有就用 4G。通话中不换，换的时候不用重启。", "Saved Wi-Fi when there is one, otherwise 4G. Not during a call; no restart.")
      : state.linkMode === "4G" ? L("一直用 4G。换上网方式马上生效，不用重启。", "Always 4G. Takes effect at once, no restart.")
      : L("只用 Wi-Fi，离开存过的网络就上不了网。", "Wi-Fi only: no connection away from saved networks.");
    const asleep = state.link === "wifi" && state.wifiNearby ? `<button class="v-line dashed is-full" data-wifi-asleep>${state.wifiAsleep ? L("假的：用一下网（叫醒 Wi-Fi）", "Fake: use the network") : L("假的：30 秒没用网，省电睡下", "Fake: idle, Wi-Fi sleeps")}</button>` : "";
    const near = `<button class="v-line dashed is-full" data-wifi-near>${state.wifiNearby ? L("假的：当作出门了（附近没有存过的 Wi-Fi）", "Fake: leave home") : L("假的：当作回到家（附近有存过的 Wi-Fi）", "Fake: come home")}</button>`;
    return `${status}${v2Fld(L("怎么上网", "Connection"), v2Seg([["auto", L("自动", "Auto")], ["4G", "4G"], ["wifi", "Wi-Fi"]], state.linkMode, "data-link"), modeNote)}${sim}
      <nav class="v-rows">${`<button class="v-row" data-go="wifi"><span><b>Wi-Fi</b><em>${state.link === "wifi" ? L("点这里选网络、换网络", "Tap to choose or change the network") : L("没在用。点这里选网络", "Not in use. Tap to choose")}</em></span>${ic("go", "chev")}</button>`}
        <button class="v-row" data-setting="proxy"><span><b>${L("代理", "Proxy")}</b><em>${proxySummary()}</em></span>${ic("go", "chev")}</button></nav>
      ${v2Fld(L("不用网时，多久断开", "Disconnect when idle after"), v2Seg([["30", L("30 秒", "30 s")], ["60", L("1 分钟", "1 min")], ["120", L("2 分钟", "2 min")], ["0", L("不断", "Never")]], state.netGrace, "data-grace"), L("断得快更省电，下一次提问要多等一两秒连网。", "Sooner saves battery; the next question waits to reconnect."))}
      <div class="v-fld">${near}${asleep}</div>`;
  }
  if (id === "proxy") {
    const onoff = [["off", L("直连", "Direct")], ["on", L("走代理", "Use proxy")]];
    return `${v2Fld(L("连 Wi-Fi 时", "On Wi-Fi"), v2Seg(onoff, state.proxyWifi ? "on" : "off", "data-proxy-wifi"))}
      ${v2Fld(L("用 4G 时", "On 4G"), v2Seg(onoff, state.proxyCell ? "on" : "off", "data-proxy-cell"), L("4G 下连不到家里局域网里的代理。", "On 4G a proxy on the home network cannot be reached."))}
      ${v2Fld(L("代理类型", "Proxy type"), v2Seg([["http", "HTTP"], ["socks5", "SOCKS5"], ["more", L("更多协议", "More")]], proxyPasswordOnly() ? "more" : state.proxyKind, "data-proxy-kind"))}
      <nav class="v-rows">${proxyPasswordOnly() ? `<button class="v-row" data-proxy-pick="proto"><span><b>${L("协议", "Protocol")}</b><em>${PROXY_PROTOCOLS[state.proxyKind]}</em></span>${ic("go", "chev")}</button>` : ""}<button class="v-row" data-proxy-addr><span><b>${L("地址", "Address")}</b><em>${state.proxyAddr || L("还没填。点这里输入", "Not set. Tap to enter")}</em></span>${ic("go", "chev")}</button>
        ${proxyPasswordOnly()
          ? `<button class="v-row" data-proxy-user><span><b>${L("密码", "Password")}</b><em>${state.proxySsPass ? L("已填", "Set") : state.proxyKind === "ss" ? L("还没填。SS 必须有密码", "Not set. SS needs one") : L("还没填。Trojan 必须有密码", "Not set. Trojan needs one")}</em></span>${ic("go", "chev")}</button>
            ${state.proxyKind === "ss"
              ? `<button class="v-row" data-proxy-pick="cipher"><span><b>${L("加密方式", "Cipher")}</b><em>${state.proxyMethod}</em></span>${ic("go", "chev")}</button>`
              : `<button class="v-row" data-proxy-sni><span><b>${L("服务器名（SNI）", "Server name (SNI)")}</b><em>${state.proxySni || L("和地址一样", "Same as the address")}</em></span>${ic("go", "chev")}</button>`}</nav>
            ${state.proxyKind === "trojan" ? v2Fld(L("检查证书", "Check the certificate"), v2Seg([["check", L("检查", "Check")], ["skip", L("不检查", "Skip")]], state.proxyInsecure ? "skip" : "check", "data-proxy-cert"),
              L("要和节点的设置一样：Surge 里写着 skip-cert-verify=true 的选「不检查」。", "Match the node: choose Skip where Surge says skip-cert-verify=true.")) : ""}`
          : `<button class="v-row" data-proxy-user><span><b>${L("账号和密码", "Login")}</b><em>${state.proxyUser ? state.proxyUser + L("，密码已填", ", password set") : L("不用。代理要账号时点这里填", "None. Tap if the proxy asks for one")}</em></span>${ic("go", "chev")}</button></nav>`}
      <div class="v-fld"><button class="v-line is-full" data-proxy-test>${L("测试这个代理", "Test this proxy")}</button>${state.proxyTest ? `<p>${state.proxyTest}</p>` : ""}
        <p>${L("不走代理的：对时、局域网里的地址、上传日志。", "Not proxied: time sync, local addresses, log upload.")}</p></div>`;
  }
  if (id === "screen") {
    const hint = { clear: L("细节最清楚，残影最少，翻页慢一点。", "Sharpest, slowest."), balanced: L("画质和速度各让一步。", "A balance of both."), fast: L("翻页最快，残影会多一些。", "Fastest, more ghosting.") }[state.refresh];
    const themes = [["slash", L("斜切", "Slash")], ["white", L("白底", "White")], ["gray", L("网点", "Dots")], ["border", L("描边", "Outline")]];
    return `${v2Fld(L("前光", "Front light"), v2Seg(Object.entries(LIGHT).map(([k, v]) => [k, pickL(v)]), state.light, "data-light") + v2Level("sun", L("亮度", "Level"), state.brightness, "bright"))}
      ${calibrationControls()}
      ${v2Fld(L("刷新", "Refresh"), v2Seg(Object.entries(REFRESH).map(([k, v]) => [k, pickL(v)]), state.refresh, "data-refresh"), hint)}
      ${v2Fld(L("待机时屏上放什么", "Standby shows"), v2Seg([["classic", L("时钟和待办", "Clock & to-dos")], ["wallpaper", L("壁纸", "Wallpaper")]], state.standbyStyle, "data-standby"), state.standbyStyle === "wallpaper" ? L("放「壁纸」里选的待机壁纸。", "Shows the standby wallpaper chosen in Wallpaper.") : L("时间、日期、天气和今天的待办。", "Time, date, weather and today's to-dos."))}
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
      <nav class="v-rows">${v2Row("pager", "", L("翻页器", "Page turner"), state.pagerOn ? "PageTurner-A · " + L("已连接", "connected") : L("正文里加 / 减音量键也能翻页", "Volume keys also turn pages"))}</nav>`;
  }
  if (id === "sound") {
    const hapt = state.haptic === "off" ? "off" : state.hapticLevel;
    return `${v2Fld(L("音量", "Volume"), v2Level("speaker", "", state.volume, "vol"))}
      ${v2Fld(L("按键震动", "Key haptics"), v2Seg([["off", L("关", "Off")], ["low", L("弱", "Low")], ["mid", L("中", "Mid")], ["high", L("强", "High")]], hapt, "data-hapt"))}
      <nav class="v-rows">${v2Row("bt", "", L("蓝牙", "Bluetooth"), L(`声音现在从${pickL(BT[state.btMode])}出 · 点这里换`, `Now: ${pickL(BT[state.btMode])} · tap to change`))}</nav>`;
  }
  if (id === "bt") {
    // 声音从哪里出放在最前面（这一页就是为它来的），然后是这个方式下要做的事，再是模块的名字和固件，复位放最后。
    const desc = { 1: L("使用本机麦克风和扬声器", "Built-in mic and speaker"), 2: L("连接蓝牙音箱或耳机播放声音", "Play through a Bluetooth speaker or headset"),
      3: L("接收手机等设备播放的声音", "Play audio from a phone here"), 5: L("向连接设备发送按键和语音", "Send keys and voice to a paired device") }[state.btMode];
    const next = { 2: L("点下面的按钮找耳机或音箱。", "Use the button below to find a headset or speaker."), 5: L("在电脑或手机的蓝牙里配对这台设备。", "Pair this device from the computer or phone.") }[state.btMode] || "";
    const action = { 2: L("找耳机或音箱", "Find a headset or speaker"), 5: L("试一下快捷键", "Try the shortcut keys") }[state.btMode];
    return `${v2Fld(L("声音从哪里出", "Audio route"), v2Seg(Object.entries(BT).map(([k, v]) => [k, pickL(v)]), state.btMode, "data-btmode").replace("v-seg", "v-seg two"), desc + (next ? "<br>" + next : ""))}
      ${action ? `<button class="v-line is-full" data-fake>${action}</button>` : ""}
      <div class="v-rows">
        <div class="v-row"><span><b>${L("蓝牙名称", "Bluetooth name")}</b><em>${esc(state.btName)}</em></span></div>
        <div class="v-row"><span><b>${L("蓝牙模块固件", "Bluetooth module firmware")}</b><em>V1.03</em></span></div>
      </div>
      <div class="v-fld"><button class="v-line is-full" data-btreset>${L("复位蓝牙模块", "Reset the Bluetooth module")}</button>
        <p>${L("连不上、没声音时试一下，几秒后自动恢复。", "Try this when nothing connects or there is no sound.")}</p></div>`;
  }
  if (id === "pager") {
    // 现在连着什么放在最上面；自动连接是一个开关；找设备、改按键在设备管理里。
    const card = state.pagerOn
      ? `<div class="v-card">${ic("check")}<span><b>PageTurner-A · ${L("已连接", "connected")}</b><em>AA:BB:CC:00:00:01</em></span></div>`
      : `<div class="v-card bad">${ic("x")}<span><b>${L("没连翻页器", "No page turner connected")}</b><em>${L("正文里加 / 减音量键也能翻页", "The volume keys also turn pages while reading")}</em></span></div>`;
    return `${card}
      ${v2Fld(L("自动连接", "Auto-connect"), v2Seg([["off", L("关", "Off")], ["on", L("开", "On")]], state.pagerAuto ? "on" : "off", "data-pager-auto"),
        L("开机及退出待机后自动寻找附近翻页器；无可用连接将自动关闭。", "Looks for page turners at start-up and after standby; turns itself off when none is found."))}
      <nav class="v-rows"><button class="v-row" data-go="pager"><span><b>${L("设备管理", "Devices")}</b><em>${L("找附近的翻页器，连接、改按键、忘记", "Find page turners nearby; connect, remap keys, forget")}</em></span>${ic("go", "chev")}</button></nav>`;
  }
  if (id === "device") {
    const speed = { 160: L("最省电", "Saver"), 240: L("均衡", "Balanced"), 320: L("最流畅", "Smooth") }[state.idleMhz];
    return `<div class="v-card">${ic("device")}<span><b>Metalio E-Ink4 Plus</b><em>${L("电量", "Battery")} ${state.battery}% · ${L("SD 卡已插入", "SD card in")}</em></span></div>
      <nav class="v-rows">
        ${v2Row("power", "", L("电池与省电", "Battery & power"), L(`${speed} · 5 分钟待机 · 充电 ${state.chargeMa / 1000} A`, `${speed} · standby after 5 min · charging ${state.chargeMa / 1000} A`))}
        ${v2Row("storage", "", L("存储与 U 盘", "Storage & USB"), state.usb ? L("电脑正在当 U 盘用", "In use by a computer as a USB disk") : L("SD 卡已插入", "SD card inserted"))}
        ${v2Row("lang", "", L("语言", "Language"), state.lang === "zh" ? "简体中文" : "English")}
        ${v2Row("diag", "", L("诊断", "Diagnostics"), L("日志、测试、关机和重启", "Logs, tests, power off and restart"))}
        ${v2Row("about", "", L("关于", "About"), "Valet 2.0.82")}
      </nav>`;
  }
  if (id === "power") {
    return `<div class="v-card">${ic("device")}<span><b>${L("电量", "Battery")} ${state.battery}%</b><em>${state.charging ? L("正在充电", "Charging") : L("没在充电", "Not charging")}</em></span></div>
      ${v2Fld(L("没操作的时候", "When idle"), v2Seg([["160", L("最省电", "Saver")], ["240", L("均衡", "Balanced")], ["320", L("最流畅", "Smooth")]], state.idleMhz, "data-mhz"), L("越省电，下一次翻页和唤醒越慢一点。", "More saving means a slightly slower page turn and wake."))}
      <div class="v-rows">
        <div class="v-row"><span><b>${L("5 分钟没操作 → 待机", "5 min idle → standby")}</b><em>${L("屏上留着日期和待办，按电源键醒来", "Date and to-dos stay on screen; the power key wakes")}</em></span></div>
        <div class="v-row"><span><b>${L("待机满 30 分钟 → 关机", "30 min in standby → off")}</b><em>${L("开机后回到上次读的那一页", "Reopens the page you were reading")}</em></span></div>
      </div>
      ${v2Fld(L("充电电流", "Charging current"), v2Seg([["500", "0.5 A"], ["1000", "1 A"], ["1500", "1.5 A"]], state.chargeMa, "data-charge"), L("充电头功率小就选 0.5 A。", "Use 0.5 A with a weak charger."))}`;
  }
  if (id === "storage") {
    // U 盘只在「传输」里开关（离开那一页会自动关），这里是卡的状态和去那里的入口。
    const card = state.usb
      ? `<div class="v-card bad">${ic("x")}<span><b>${L("SD 卡 · 电脑在用", "SD card · in use by a computer")}</b><em>${L("电脑上弹出 U 盘后，这里才能用卡", "Eject it on the computer to use the card here")}</em></span></div>`
      : `<div class="v-card">${ic("device")}<span><b>${L("SD 卡 · 已插入", "SD card · inserted")}</b><em>${L("还剩 14.5 GB，共 14.8 GB", "14.5 GB free of 14.8 GB")}</em></span></div>`;
    return `${card}
      <nav class="v-rows"><button class="v-row" data-go="transfer"><span><b>${L("当 U 盘用", "Use as a USB disk")}</b><em>${L("打开「传输」的 U 盘一栏，接上电脑拷文件", "Opens the USB disk tab of Transfer; copy files from a computer")}</em></span>${ic("go", "chev")}</button></nav>`;
  }
  if (id === "lang") {
    return v2Fld(L("界面语言", "Language"), v2Seg([["zh", "简体中文"], ["en", "English"]], state.lang, "data-lang"), L("切换后立即生效", "Takes effect immediately"));
  }
  if (id === "diag") {
    const pair = (items) => `<div class="v-two">${items.map((name) => `<button class="v-line" data-fake>${name}</button>`).join("")}</div>`;
    return `${v2Fld(L("运行日志", "Runtime log"), v2Seg([["on", L("记录", "Record")], ["off", L("不记录", "Off")]], state.logOn ? "on" : "off", "data-log"),
        L("记在 SD 卡的 metalio/e-ink/logs 里。<br>出了问题时上传，方便查原因。", "Kept on the SD card in metalio/e-ink/logs.<br>Upload it when something goes wrong."))}
      ${pair([L("上传日志", "Upload logs"), L("清除日志", "Clear logs")])}
      <div class="v-fld"><label>${L("测试", "Tests")}</label>${pair([L("自动测试", "Auto test"), L("触摸测试", "Touch test"), L("电池测试", "Battery test"), L("老化测试", "Aging test"), L("显示测试", "Display test"), L("UI调节", "UI adjustment")])}</div>
      <div class="v-fld"><label>${L("设备操作", "Device")}</label>${pair([L("关机", "Power off"), L("重启", "Restart")])}</div>`;
  }
  // 关于：先说这是什么，再是几行平实的信息。地址是假的。
  return `<div class="v-card">${ic("device")}<span><b>${L("Valet 随行", "Valet")}</b><em>${L("固件", "Firmware")} 2.0.82 · Nov 20 2026</em></span></div>
    <nav class="v-rows"><button class="v-row" data-fake><span><b>${L("检查更新", "Check for updates")}</b><em>${L("查的是官方固件的版本，有新版本会先问你", "Checks the official firmware's version; asks before installing")}</em></span>${ic("go", "chev")}</button>
      <div class="v-row"><span><b>${L("设备型号", "Device model")}</b><em>metalio-e-ink4-plus</em></span></div>
      <div class="v-row"><span><b>${L("芯片", "Chip")}</b><em>ESP32-S31 · ${L("2 核", "2 cores")}</em></span></div>
      <div class="v-row"><span><b>${L("编译时间", "Build time")}</b><em>Nov 20 2026 15:33:15</em></span></div>
      <div class="v-row"><span><b>${L("MAC 地址", "MAC")}</b><em>00:00:00:00:00:00</em></span></div>
      <div class="v-row"><span><b>${L("存储器", "Memory")}</b><em>Flash 16.0 MB · PSRAM 16.0 MB</em></span></div></nav>`;
}
// 网络页「代理」一行的说明：没设置 / 没开 / 在用 / 只在另一种上网方式下才走。
// SS 和 Trojan 只有一个密码，没有账号（和设备上 Prefs::PasswordOnly 一样）。
// 上网方式。state.linkMode 是选的，state.link 是现在在用的。自动：有存过的 Wi-Fi 就用 Wi-Fi，没有就 4G。
// 真机上：用 Wi-Fi 时连不上就转 4G；用 4G 时空闲每隔几分钟扫一次 Wi-Fi，扫到存过的就换回来。都不重启，通话中不换。
function linkWanted() {
  return state.linkMode === "auto" ? (state.wifiNearby ? "wifi" : "4G") : state.linkMode;
}
function settleLink(why) {
  const want = linkWanted();
  const name = want === "4G" ? "4G" : "Wi-Fi";
  if (want === state.link) { state.linkPending = ""; return; }
  if (state.live) {
    state.linkPending = want;
    log(`${why}：正在通话，先不换，挂断后换到 ${name}。`);
    return;
  }
  state.link = want;
  state.linkPending = "";
  if (want === "wifi" && !state.wifiNearby) toast(L("只用 Wi-Fi：附近没有存过的网络", "Wi-Fi only: no saved network nearby"));
  else toast(L(`已切换到${name}`, `Switched to ${name}`));
  log(`${why}：换到 ${name}，不重启；正在用的连接自己重连。`);
}
function proxyPasswordOnly() { return state.proxyKind === "ss" || state.proxyKind === "trojan"; }
function proxySummary() {
  if (!state.proxyAddr) return L("没设置。点这里填", "Not set. Tap to set up");
  const where = `${state.proxyKind === "socks5" ? "SOCKS5" : state.proxyKind === "ss" ? "SS" : state.proxyKind === "trojan" ? "Trojan" : "HTTP"} ${state.proxyAddr}`;
  const wifi = state.link === "wifi";
  if (wifi ? state.proxyWifi : state.proxyCell) return L("在用 · ", "In use · ") + where;
  if (!state.proxyWifi && !state.proxyCell) return L("没开 · ", "Off · ") + where;
  return (state.proxyWifi ? L("连 Wi-Fi 时才走 · ", "Only on Wi-Fi · ") : L("用 4G 时才走 · ", "Only on 4G · ")) + where;
}
// 「更多协议」里的协议，和 SS 的加密方式：都放在弹出列表里选，不分组、不评价（用哪种由服务器决定）。
// 列表高度是整行的倍数，拖完停在整行上；项少时面板缩到刚好放下。打开时正在用的那项尽量在中间。点一项就选上并关掉。
const PROXY_PROTOCOLS = { ss: "SS", trojan: "Trojan" };
const SS_METHODS = ["aes-128-gcm", "aes-192-gcm", "aes-256-gcm", "chacha20-ietf-poly1305", "xchacha20-ietf-poly1305",
  "aes-128-cfb", "aes-192-cfb", "aes-256-cfb", "aes-128-ctr", "aes-192-ctr", "aes-256-ctr", "chacha20-ietf"];
const PICK_ROW = 31, PICK_ROWS = 8;
function proxyPicker() {
  const proto = state.proxyPicker === "proto";
  const items = proto ? Object.entries(PROXY_PROTOCOLS) : SS_METHODS.map((m) => [m, m]);
  const current = proto ? state.proxyKind : state.proxyMethod;
  const shown = Math.min(PICK_ROWS, items.length), scrolls = items.length > shown;
  const at = items.findIndex(([v]) => v === current);
  const top = Math.max(0, Math.min(at - (shown >> 1), items.length - shown)) * PICK_ROW;
  const rows = items.map(([v, name]) => `<button class="v-pick-row${v === current ? " on" : ""}" data-proxy-choose="${v}"><b>${name}</b>${v === current ? `<em>${L("在用", "In use")}</em>` : ""}</button>`).join("");
  setTimeout(() => { const list = panel.querySelector(".v-pick-list"); if (list) list.scrollTop = top; });
  const note = scrolls ? L("要和服务器的设置一样。列表可以上下拖。", "Must match the server. Drag the list up and down.") : L("要和服务器的设置一样。", "Must match the server.");
  return `<div class="v-pick" data-pick-mask><div class="v-pick-panel${scrolls ? "" : " fit"}">
      <div class="v-pick-head"><b>${proto ? L("协议", "Protocol") : L("加密方式", "Cipher")}</b><button data-pick-close>${L("关闭", "Close")}</button></div>
      <p>${note}</p>
      <div class="v-pick-list${scrolls ? "" : " fit"}" style="height:${PICK_ROW * shown}px">${rows}</div></div></div>`;
}

function v2Settings() {
  const id = state.settingsScreen;
  const titles = {
    list: L("设置", "Settings"), talk: L("助手", "Assistant"), net: L("网络", "Network"), screen: L("显示", "Display"),
    read: L("阅读与字库", "Reading & fonts"), sound: L("声音与蓝牙", "Sound & Bluetooth"), device: L("设备", "Device"),
    power: L("电池与省电", "Battery & power"), storage: L("存储与 U 盘", "Storage & USB"), lang: L("语言", "Language"),
    diag: L("诊断", "Diagnostics"), about: L("关于", "About"), proxy: L("代理", "Proxy"),
    bt: L("蓝牙", "Bluetooth"), pager: L("翻页器", "Page turner"),
  };
  const body = id === "list" ? v2SettingsHome() : v2SettingsPane(id);
  const picker = id === "proxy" && proxyPasswordOnly() && (state.proxyPicker === "proto" || (state.proxyPicker === "cipher" && state.proxyKind === "ss")) ? proxyPicker() : "";
  return `${bar()}<main class="v-page v-set">${v2Head(titles[id] || titles.list)}<div class="v-pane${id === "net" ? " is-tight" : ""}">${body}</div></main>${picker}`;
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
  // 快捷面板：开着时只处理面板里的点按，点面板外收起
  if (state.quick) {
    if (hit("[data-quick-close]") || hit("[data-quick-open]")) { state.quick = false; return render(); }
    if ((el = hit("[data-quick]"))) {
      const k = el.dataset.quick;
      if (k === "link") {
        // 和设置页同一套：自动 → 4G → Wi-Fi → 自动
        state.linkMode = { auto: "4G", "4G": "wifi", wifi: "auto" }[state.linkMode];
        settleLink(L(`快捷面板：上网方式改成「${{ auto: "自动", "4G": "4G", wifi: "Wi-Fi" }[state.linkMode]}」`, "Quick panel"));
      } else if (k === "proxy") {
        if (!state.proxyAddr || (proxyPasswordOnly() && !state.proxySsPass)) { state.quick = false; return openSetting("proxy"); }
        const key = state.link === "wifi" ? "proxyWifi" : "proxyCell";
        state[key] = !state[key];
        toast(state[key] ? L("代理已开启", "Proxy on") : L("代理已关闭", "Proxy off"));
        log(`快捷面板：${state.link === "wifi" ? "连 Wi-Fi 时" : "用 4G 时"}${state[key] ? "走代理" : "直连"}（另一条网的开关不动）。`);
      }
      return render();
    }
    // 一排左边的图标：前光关 / 开、静音 / 有声。关掉时记着原来的档位，打开时回到那一档。
    if ((el = hit("[data-level-off]"))) {
      if (el.dataset.levelOff === "bright") {
        const off = state.light === "off" || state.brightness === 0;
        if (off) { state.light = "both"; if (state.brightness === 0) state.brightness = state.brightnessBefore || 40; }
        else state.light = "off";
        log(off ? `快捷面板：前光打开，回到 ${state.brightness}。` : "快捷面板：前光关掉（档位记着）。");
      } else {
        if (state.volume === 0) state.volume = state.volumeBefore || 60;
        else { state.volumeBefore = state.volume; state.volume = 0; }
        log(state.volume ? `快捷面板：取消静音，回到 ${state.volume}。` : "快捷面板：静音（档位记着）。");
      }
      return render();
    }
    if (hit("[data-level]")) {
      el = hit("[data-level]");
      const [key, n] = el.dataset.level.split(":");
      const value = Number(n) * 10;
      if (key === "bright") {
        if (state.light === "off") state.brightness = value;   // 关着时点格子：直接开到那一档
        else state.brightness = state.brightness === value ? value - 10 : value;
        if (state.brightness > 0) state.brightnessBefore = state.brightness;
        state.light = state.brightness > 0 ? (state.light === "off" ? "both" : state.light) : "off";
      }
      else if (key === "warm") state.warmth = state.warmth === value ? value - 10 : value;
      else { state.volume = state.volume === value ? value - 10 : value; if (state.volume > 0) state.volumeBefore = state.volume; }
      return render();
    }
    if (hit(".q-panel")) return;
    state.quick = false; return render();
  }
  if (hit("[data-quick-open]")) { state.quick = true; log("点了状态栏（真机上是从顶上往下拉）：出快捷面板。"); return render(); }
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
  if (hit("[data-float-close]")) return void v2CloseFloat("点了 ✕");
  if (hit("[data-float-expand]")) return v2ExpandFloat();
  if (hit("[data-ask]")) return v2Ask();
  if (hit("[data-live]")) return state.float ? v2OpenFloat(true) : v2GoLive();
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
  if ((el = hit("[data-pager-auto]"))) { state.pagerAuto = el.dataset.pagerAuto === "on"; return render(); }
  if (hit("[data-fake]")) return toast(L("假壳子里这一步不做", "Not done in the mock-up"));
  // 代理页。设备上地址和账号用屏幕键盘输入（账号、密码各一个框）；这里点一下填上示例，再点一下清空。
  if ((el = hit("[data-proxy-wifi]")) || (el = hit("[data-proxy-cell]"))) {
    const wifi = "proxyWifi" in el.dataset;
    const on = (wifi ? el.dataset.proxyWifi : el.dataset.proxyCell) === "on";
    if (on && !state.proxyAddr) return toast(L("先填代理的地址", "Enter the proxy address first"));
    if (on && proxyPasswordOnly() && !state.proxySsPass) return toast(state.proxyKind === "ss" ? L("先填 SS 的密码", "Enter the SS password first") : L("先填 Trojan 的密码", "Enter the Trojan password first"));
    state[wifi ? "proxyWifi" : "proxyCell"] = on;
    state.proxyTest = "";
    return render();
  }
  if ((el = hit("[data-proxy-choose]"))) {
    const v = el.dataset.proxyChoose;
    if (state.proxyPicker === "proto") state.proxyKind = state.proxyMore = v; else state.proxyMethod = v;
    state.proxyPicker = false; state.proxyTest = ""; return render();
  }
  if ((el = hit("[data-proxy-pick]"))) { state.proxyPicker = el.dataset.proxyPick; return render(); }
  // 点「关闭」或点面板外面（遮罩本身）都关掉列表。
  if (hit("[data-pick-close]") || e.target.matches("[data-pick-mask]")) { state.proxyPicker = false; return render(); }
  if (hit(".v-pick")) return;
  // 「更多协议」回到上次用的那种（没用过时是 SS）；具体哪种在「协议」那一行里换。
  if ((el = hit("[data-proxy-kind]"))) { const k = el.dataset.proxyKind; state.proxyKind = k === "more" ? state.proxyMore : k; state.proxyTest = ""; return render(); }
  if (hit("[data-wifi-asleep]")) {
    state.wifiAsleep = !state.wifiAsleep;
    log(state.wifiAsleep ? "假的：30 秒没用网，Wi-Fi 省电睡下。状态栏扇形变灰，下拉栏写「休眠中」；一用网会自己连上。" : "假的：用了一下网，Wi-Fi 醒来连上。");
    return render();
  }
  if (hit("[data-wifi-near]")) {
    state.wifiAsleep = false;
    state.wifiNearby = !state.wifiNearby;
    if (state.linkMode !== "auto") {
      log(state.wifiNearby ? "假的：回到有存过的 Wi-Fi 的地方。没选「自动」，不换。" : "假的：出门了。没选「自动」，不换。");
      return render();
    }
    settleLink(state.wifiNearby ? "空闲时扫到存过的 Wi-Fi（Metalio-Home）" : "Wi-Fi 连不上了");
    return render();
  }
  if ((el = hit("[data-proxy-cert]"))) { state.proxyInsecure = el.dataset.proxyCert === "skip"; state.proxyTest = ""; return render(); }
  if (hit("[data-proxy-sni]")) {
    state.proxySni = state.proxySni ? "" : "cdn.example.com";
    state.proxyTest = "";
    log(state.proxySni ? "假的：当作在键盘上输入了 Trojan 的服务器名（SNI）。" : "假的：清空了服务器名，TLS 里用地址。");
    return render();
  }
  if (hit("[data-proxy-addr]")) {
    state.proxyAddr = state.proxyAddr ? "" : "192.168.1.20:7890";
    if (!state.proxyAddr) state.proxyWifi = state.proxyCell = false;
    state.proxyTest = "";
    log(state.proxyAddr ? "假的：当作在键盘上输入了代理地址。" : "假的：清空了代理地址，两个开关跟着关掉。");
    return render();
  }
  if (hit("[data-proxy-user]") && proxyPasswordOnly()) {
    state.proxySsPass = !state.proxySsPass;
    state.proxyTest = "";
    const name = state.proxyKind === "ss" ? "SS" : "Trojan";
    log(state.proxySsPass ? `假的：当作在密码框里输入了 ${name} 的密码（只有一个框，没有账号）。` : `假的：清空了 ${name} 的密码。`);
    return render();
  }
  if (hit("[data-proxy-user]")) {
    state.proxyUser = state.proxyUser ? "" : "valet";
    state.proxyTest = "";
    log(state.proxyUser ? "假的：当作在两个框里输入了账号和密码。" : "假的：清空了账号和密码。");
    return render();
  }
  if (hit("[data-proxy-test]")) {
    if (!state.proxyAddr) return toast(L("先填代理的地址", "Enter the proxy address first"));
    state.proxyTest = state.link === "4G"
      ? L("连不上代理。看看地址和端口对不对，代理有没有允许别的设备来连。", "Cannot reach the proxy. Check the address and port, and that it accepts other devices.")
      : L("通了：经代理连到 GitHub 用了 1.1 秒。", "Works: reached GitHub through the proxy in 1.1 s.");
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
      // 关着时点格子：直接开到那一档（和下拉栏、真机一致）
      if (state.light === "off") state.brightness = value;
      else state.brightness = state.brightness === value ? value - 10 : value;
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

// 快捷面板里长按开关进对应设置（模拟界面里用右键代替长按）
panel.addEventListener("contextmenu", (e) => {
  const tile = isV2() && state.quick && e.target.closest("[data-quick]");
  if (!tile) return;
  e.preventDefault();
  state.quick = false;
  const target = { link: "net", proxy: "proxy" }[tile.dataset.quick];
  log(`长按快捷面板的开关：进设置 → ${target}。`);
  openSetting(target);
});
