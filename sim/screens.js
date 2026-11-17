// 每个界面一个函数，返回面板里的 HTML。真屏 684×1216，这里缩到 246×438。
const NAMES = {
  home: "首页", detail: "书的详情", reader: "正文", ai: "百问",
  settings: "设置", wallpaper: "壁纸", tasks: "每日清单", transfer: "传输",
};

function esc(text) {
  return String(text).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);
}

function bar(center = "10:48") {
  const net = state.link === "4G" ? "4G" : "Wi-Fi";
  return `<div class="bar"><span><i class="batt"></i>70%${state.usb ? " · USB" : ""}</span><span class="c">${center}</span><span>${net}</span></div>`;
}

// 首页六格，照固件的斜切排法（home_apps_slash.cc）：第 1、3 行左格上宽下窄，第 2 行反过来。
const TILES = [
  ["detail", "阅读", "book", true],
  ["wallpaper", "壁纸", "wallpaper", false],
  ["ai", "百问AI", "teacher", false],
  ["tasks", "每日清单", "task", true],
  ["transfer", "传输", "cloud", true],
  ["settings", "设置", "setting", false],
];

function tilePoints(row, side) {
  // 量自官方正面渲染图：每行高 64、行距 6；斜边上下端 x 是 140 和 96，右格往右错 7。
  const y0 = row * 70;
  const y1 = y0 + 64;
  const [top, bottom] = row === 1 ? [96, 140] : [140, 96];
  if (side === 0) return `12,${y0} ${top},${y0} ${bottom},${y1} 12,${y1}`;
  return `${top + 7},${y0} 234,${y0} 234,${y1} ${bottom + 7},${y1}`;
}

function home() {
  const tiles = TILES.map(([go, name, icon, dark], i) => {
    const row = Math.floor(i / 2);
    const side = i % 2;
    const y = row * 70 + 32;
    const label = side === 0
      ? `<text x="68" y="${y + 4.5}">${name}</text>`
      : `<text x="180" y="${y + 4.5}" text-anchor="end">${name}</text>`;
    const icon_ = `<image href="assets/ic_app_home_${icon}.png" x="${side === 0 ? 22 : 191}" y="${y - 16}" width="32" height="32"${dark ? ' filter="url(#invert)"' : ""} />`;
    return `<g class="tile${dark ? " dark" : ""}" data-go="${go}"><polygon points="${tilePoints(row, side)}" />${icon_}${label}</g>`;
  }).join("");
  return `${bar()}<div class="hero">
      <span class="hh">10</span><i class="slash"></i><span class="mm">48</span>
      <div class="date"><i></i>2026.7.13<b>五月廿九</b><span>星期二</span></div>
    </div>
    <svg class="tiles" viewBox="0 0 246 206" width="246" height="206">
      <filter id="invert"><feColorMatrix type="matrix" values="-1 0 0 0 1 0 -1 0 0 1 0 0 -1 0 1 0 0 0 1 0" /></filter>
      ${tiles}
    </svg>`;
}

function detail() {
  return `${bar()}<div class="detail">
    <div class="cover">三国演义</div>
    <h3>三国演义</h3>
    <p class="note">罗贯中 · TXT · 存在卡上</p>
    <p class="note">读到第 ${state.page + 1} / ${pages.length} 页</p>
    <button class="wide" data-go="reader">继续阅读</button>
  </div>`;
}

function reader() {
  const body = pages[state.page].map((p) => `<p>${p}</p>`).join("");
  const sheet = state.sheet ? `<div class="sheet">
      <div class="tabs"><span>目录</span><span>字体</span><span>书签</span><span class="on">设置</span></div>
      <div class="row-btns"><button>减小字号</button><button>增加字号</button></div>
      <p class="note">卡片开着时，音量键调音量，不翻页。</p>
    </div>` : "";
  return `<div class="read">
    <div class="read-head"><span>三国演义</span><span>第一回</span></div>
    <div class="read-body">${body}</div>
    <div class="read-foot"><span>10:48 · 70%</span><span>${state.page + 1} / ${pages.length}</span></div>
    ${sheet}
  </div>`;
}

function simple(title, lines) {
  return `${bar()}<div class="simple"><h3>${title}</h3>${lines.map((l) => `<p>${l}</p>`).join("")}</div>`;
}
function wallpaper() {
  return simple("壁纸", ["待机和关机可以各放一张全屏图。假壳子里是空的。"]);
}
function tasks() {
  return simple("每日清单", ["还没有待办。官方百问可以用语音往这里加。"]);
}
function transfer() {
  return simple("传输", [
    "扩展字库不在固件里，要放到卡上：<code>metalio/e-ink/fonts/extended-fonts.fontpack</code>",
    "只对 misans_20 到 misans_50 这几档 .ef 做缺字回退，自选 TTF 不走这条。",
    "假的，不会真的传文件。",
  ]);
}

// 待机用经典时钟样式；关机画面直接用官方固件里的 bg_shutdown_zh.png。
function standbyScreen() {
  return `<div class="standby"><div class="hm">10<br>48</div><div class="date">2026.7.13　五月廿九　星期二</div></div>`;
}
