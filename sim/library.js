// 新设计 · 书库（OPDS 订阅）。书架不分栏：SD 卡上的书和订阅书库里的书放在一起，
// 来自书库的书在封面右上角斜着贴一条来源（「书斋」「古登堡」），没贴的就是本机自己的书。
// 书库的书没下载时封面是虚线框，下面写「未下载」。书架上只放书库的在读、想读和最近加入，其余的靠搜。
// 订阅可以有多个（别的用户可能订好几个 OPDS），书斋这类书库还带「在读 / 想读」和阅读进度。
// 设备键盘打不了中文，所以搜书时拼音首字母也算（hlm → 红楼梦），英文书名照常打。
// 书斋会把上游书源（维基文库、古登堡）的结果放进同一个搜索：搜到书斋还没收的书，书角标上游的名字，
// 点下载时书斋先把书收进书库（take → 轮询 job，约 6 秒），再照常下载、同步进度。只在搜书时出现。
// 假数据：书斋 · 杜万 一个订阅；设置里可以假装再加一个古登堡，看多个订阅时每本书怎么标来源。

const LIB_BOOKS = [
  { id: 101, lib: "weread", title: "风景与权力", author: "W.J.T.米切尔", py: "fjyql", fmt: "EPUB", size: "4.8 MB", status: "reading", progress: 0.42, wish: false, recent: true,
    desc: "从文化景观与视觉权力的角度审视风景绘画与帝国记忆。" },
  { id: 3, lib: "shuzhai", title: "围城", author: "钱钟书", py: "wc qianzhongshu", fmt: "EPUB", size: "1.2 MB", status: "reading", progress: 0.35, wish: false, recent: true,
    desc: "方鸿渐留学归来，在婚姻和职业里进进出出。" },
  { id: 102, lib: "weread", title: "三体", author: "刘慈欣", py: "st liucixin", fmt: "EPUB", size: "2.4 MB", status: "reading", progress: 0.68, wish: false, recent: true,
    desc: "红岸基地向宇宙发出的第一声啼鸣，引来了四光年外的三体舰队。" },
  { id: 7, lib: "shuzhai", title: "长安的荔枝", author: "马伯庸", py: "cadlz mabaiyong", fmt: "EPUB", size: "860 KB", status: "reading", progress: 0.18, wish: true, recent: true,
    desc: "唐天宝十四年，上林署监事李善德接到一桩任务：在贵妃诞日之前，从岭南运来新鲜荔枝。" },
  { id: 103, lib: "weread", title: "置身事内", author: "兰小欢", py: "zssn lanxiaohuan", fmt: "EPUB", size: "1.5 MB", status: "unread", progress: 0, wish: true, recent: true,
    desc: "中国政府与经济发展：地方政府如何推动土地财政与产业投资。" },
  { id: 9, lib: "shuzhai", title: "活着", author: "余华", py: "hz yuhua", fmt: "EPUB", size: "420 KB", status: "finished", progress: 1, wish: false, recent: false,
    desc: "地主少爷福贵的一生。" },
  { id: 12, lib: "shuzhai", title: "小王子", author: "圣埃克苏佩里", py: "xwz", fmt: "EPUB", size: "1.6 MB", status: "unread", progress: 0, wish: true, recent: true,
    desc: "一个住在小行星上的王子，离开他的玫瑰去旅行。" },
  { id: 15, lib: "shuzhai", title: "明朝那些事儿", author: "当年明月", py: "mcnxse dangnianmingyue", fmt: "MOBI", size: "5.1 MB", status: "unread", progress: 0, wish: false, recent: true, convert: true,
    desc: "从朱元璋出生讲到明朝灭亡，讲人，也讲制度。" },
  { id: 18, lib: "shuzhai", title: "人间词话", author: "王国维", py: "rjch wangguowei", fmt: "EPUB", size: "340 KB", status: "reading", progress: 0.08, wish: false, recent: false,
    desc: "以「境界」论词，从李白、温庭筠一路讲到纳兰性德。" },
  { id: 9001, lib: "wikisource", via: "shuzhai", title: "The Art of War", author: "Sun Tzu", py: "", fmt: "EPUB", size: "210 KB",
    desc: "Lionel Giles 1910 年的英译本，十三篇。" },
  { id: 9002, lib: "wikisource", via: "shuzhai", title: "The Analects", author: "Confucius", py: "", fmt: "EPUB", size: "380 KB",
    desc: "James Legge 的英译本。" },
  { id: 2701, lib: "gutenberg", title: "Moby Dick", author: "Herman Melville", py: "", fmt: "EPUB", size: "1.3 MB", recent: true,
    desc: "Ishmael's voyage on the whaler Pequod, commanded by Captain Ahab." },
  { id: 1342, lib: "gutenberg", title: "Pride and Prejudice", author: "Jane Austen", py: "", fmt: "EPUB", size: "780 KB", recent: true,
    desc: "Elizabeth Bennet and Mr Darcy." },
];
const LIB_SOURCES = {
  weread: { id: "weread", name: "微信读书", short: "微读", url: "https://weread.qq.com", rich: true },
  shuzhai: { id: "shuzhai", name: "书斋 · 杜万", short: "书斋", url: "https://books.visionclaw.online/opds/k3m9p4", rich: true },
  gutenberg: { id: "gutenberg", name: "古登堡", short: "古登堡", url: "https://www.gutenberg.org/ebooks.opds/", rich: false },
  wikisource: { id: "wikisource", name: "维基文库（英文）", short: "维基文库", upstream: true },
};
Object.assign(state, {
  libs: ["weread", "shuzhai"], // 已订阅的书库（含微信读书与书斋）
  libQuery: "",
  libBook: null,          // 正在看详情的那本
  libGot: { 101: true, 3: true, 102: true, 7: true }, // 已下载到本机：id → true；正在下：id → 0..1
  libConvert: {},         // 正在转 EPUB：id → true
  libTake: {},            // 书斋正在从上游收这本：id → true
  libAdding: false,       // 设置里正在填新订阅
  libOpds: false,         // 是否在看书库浏览页（固件 kScreenOpds）
  libOpdsBrowse: null,    // 当前书库浏览层：{title, entries:[{title,nav,files,size}], page}
  libOpdsQuery: "",       // 书库搜索框里的词
});

function libOn() { return state.libs.map((id) => LIB_SOURCES[id]); }
// 上游的书挂在书斋名下：订了书斋才有，而且只在搜书时出现。
function libBooks(searching) { return LIB_BOOKS.filter((b) => state.libs.includes(b.via || b.lib) && (searching || !b.via)); }
// 本机的假书（screens.js 的 BOOKS）补上拼音，好和书库的书一起搜。
const LOCAL_PY = { 三国演义: "sgyy luoguanzhong", 红楼梦: "hlm caoxueqin", 西游记: "xyj wuchengen" };
function libMatch(b, k) {
  return b.title.toLowerCase().includes(k) || b.author.toLowerCase().includes(k) || (b.py && b.py.split(" ").some((p) => p.startsWith(k)));
}
function libStatus(b) {
  const got = state.libGot[b.id];
  if (got === true) return L("在本机", "On device");
  if (typeof got === "number") return L(`下载中 ${Math.round(got * 100)}%`, `Downloading ${Math.round(got * 100)}%`);
  if (state.libConvert[b.id]) return L("书斋在转 EPUB…", "Converting…");
  if (state.libTake[b.id]) return L("书斋在收录…", "Adding…");
  if (b.via) return L("书斋可收", "Can add");
  if (b.status === "reading") return L(`在读 ${Math.round(b.progress * 100)}%`, `Reading ${Math.round(b.progress * 100)}%`);
  if (b.status === "finished") return L("读完了", "Finished");
  if (b.wish) return L("想读", "Want to read");
  return "";
}

// 书架上的一本：本机的书没有 src；书库的书带 src（贴斜标签），没下载的 away=true（虚线框）。
const LOCAL_PROG = { 三国演义: 0.24, 红楼梦: 0.06, 西游记: 0 };
function shelfItems(query) {
  const k = query.trim().toLowerCase();
  const local = BOOKS.map((b, i) => {
    const p = LOCAL_PROG[b.title] || 0;
    return {
      id: `local-${i}`, title: b.title, author: b.author, py: LOCAL_PY[b.title] || "", fmt: b.format, size: b.size,
      attr: `data-book="${i}"`, rank: b.seconds !== "0m" ? 0 : 2,
      progress: p,
      note: p > 0 ? L(`${Math.round(p * 100)}%`, `${Math.round(p * 100)}%`) : b.format,
    };
  });
  const lib = libBooks(!!k).filter((b) => k || state.libGot[b.id] === true || b.status === "reading" || (b.wish && b.status !== "finished") || b.recent)
    .map((b) => {
      const here = state.libGot[b.id] === true;
      const st = libStatus(b);
      const p = typeof b.progress === "number" ? b.progress : (b.status === "finished" ? 1 : 0);
      return {
        id: b.id, title: b.title, author: b.author, py: b.py, fmt: b.fmt, size: b.size,
        attr: `data-lib-book="${b.id}"`, src: LIB_SOURCES[b.lib].short, srcId: b.lib,
        away: !here, progress: p,
        rank: b.via ? 4 : b.status === "reading" ? 0 : b.wish ? 1 : 3,
        note: here ? (p > 0 ? L(`${Math.round(p * 100)}%`, `${Math.round(p * 100)}%`) : L("在本机", "Here")) : (st || L("未下载", "Not here")),
      };
    });
  const all = [...local, ...lib];
  return (k ? all.filter((b) => libMatch(b, k)) : all).sort((x, y) => x.rank - y.rank);
}

// 封面上中文最多 6 个字；英文按词截，最多 14 个字母。
function libCoverTitle(t) {
  if (/[\u4e00-\u9fff]/.test(t)) return t.length > 6 ? t.slice(0, 6) : t;
  return t.length > 14 ? t.slice(0, 14).replace(/\s+\S*$/, "") + "…" : t;
}
function libTagClass(b) {
  if (b.srcId === "weread" || b.src === "微读") return " is-dark";
  if (b.srcId === "shuzhai" || b.src === "书斋") return " is-paper";
  return " is-dither";
}
function libTag(b) {
  return b.src ? `<i class="v-ltag${libTagClass(b)}">${esc(b.src)}</i>` : "";
}

// 针对中文两字/四字书名的折行美化：例如「三国演义」→「三国 / 演义」，「长安的荔枝」→「长安的 / 荔枝」
function splitTitleLines(t) {
  const s = libCoverTitle(t);
  if (!/[\u4e00-\u9fff]/.test(s)) return [s];
  if (s.length === 4) return [s.slice(0, 2), s.slice(2)];
  if (s.length === 5) return [s.slice(0, 3), s.slice(3)];
  if (s.length === 6) return [s.slice(0, 3), s.slice(3)];
  return [s];
}

function pickCoverTpl(b) {
  if (state.coverTpl && state.coverTpl !== "auto") return state.coverTpl;
  if (b.away) return "classic";
  // 按书名稳定分配 4 种高级书籍装帧模板：
  // 0: tategaki (中式竖排线装签条)
  // 1: dark (深墨典藏反白细框)
  // 2: slash (Valet 标志性 11.2° 斜切撞色)
  // 3: classic (经典左书脊排版)
  const map = {
    "三国演义": "tategaki",
    "红楼梦": "tategaki",
    "西游记": "classic",
    "人间词话": "tategaki",
    "长安的荔枝": "tategaki",
    "风景与权力": "slash",
    "三体": "dark",
    "围城": "dark",
    "置身事内": "slash",
    "小王子": "classic",
    "明朝那些事儿": "slash",
  };
  if (map[b.title]) return map[b.title];
  let h = 0;
  for (let i = 0; i < b.title.length; i++) h = (h * 31 + b.title.charCodeAt(i)) & 0xffff;
  return ["tategaki", "dark", "slash", "classic"][h % 4];
}

function libCover(b, cls = "") {
  const tpl = cls === " sm" ? "classic" : pickCoverTpl(b);
  const tag = cls === " sm" ? "" : libTag(b);
  const lines = splitTitleLines(b.title).map((l) => `<span>${esc(l)}</span>`).join("");
  const author = esc(b.author || "");
  return `<i class="v-lcover${cls} tpl-${tpl}${b.away ? " away" : ""}${b.src ? " tagged" : ""}">
    <span class="v-cov-in">
      <b class="v-cov-t">${lines}</b>
      <em class="v-cov-a">${author}</em>
    </span>
    ${tag}
  </i>`;
}

function libDetail(b) {
  const src = LIB_SOURCES[b.lib];
  const got = state.libGot[b.id];
  let main;
  if (got === true) main = `<button class="v-solid" data-lib-read="${b.id}">${b.progress ? L(`接着读 · ${Math.round(b.progress * 100)}%`, "Continue") : L("开始阅读", "Read")}</button>`;
  else if (typeof got === "number") main = `<button class="v-solid" disabled>${L(`下载中 ${Math.round(got * 100)}%`, `Downloading ${Math.round(got * 100)}%`)}</button>`;
  else if (state.libTake[b.id]) main = `<button class="v-solid" disabled>${L("书斋在收录…", "Adding to the library…")}</button>`;
  else if (b.via) main = `<button class="v-solid" data-lib-get="${b.id}">${L("收进书斋，再下载", "Add to library, then download")}</button>`;
  else if (state.libConvert[b.id]) main = `<button class="v-solid" disabled>${L("书斋在转 EPUB…", "Converting…")}</button>`;
  else if (b.convert) main = `<button class="v-solid" data-lib-get="${b.id}">${L("转成 EPUB 再下载", "Convert, then download")}</button>`;
  else main = `<button class="v-solid" data-lib-get="${b.id}">${L("下载到书架", "Download")} · ${b.size}</button>`;
  const wish = src.rich && !b.via && b.status !== "finished"
    ? `<button class="v-line${b.wish ? " on" : ""}" data-lib-wish="${b.id}">${b.wish ? L("已在想读 · 取消", "In list · remove") : L("加到想读", "Want to read")}</button>` : "";
  const prog = src.rich && b.progress ? `<p class="v-lhint">${L(`读到 ${Math.round(b.progress * 100)}%，在这台设备或网页上读都会接着。`, "Progress syncs with your other devices.")}</p>` : "";
  return `<div class="v-lbook">
      <div class="v-lbook-top">${libCover(b, " big")}<div><h3>${esc(b.title)}</h3><p>${esc(b.author)}</p>
        <p class="v-lmeta">${L("来自", "From")}「${esc(src.short)}」${b.via ? L("，经书斋", " via 书斋") : ""} · ${b.fmt} · ${b.size}</p>${libStatus(b) ? `<p class="v-lmeta"><b>${libStatus(b)}</b></p>` : ""}</div></div>
      <p class="v-ldesc">${esc(b.desc)}</p>${prog}${b.via ? `<p class="v-lhint">${L("书斋里还没有这本。点下面让书斋从维基文库收进书库，收好自动下载；以后它就是书斋里的书，进度照常同步。", "Not in 书斋 yet. It is added first, then downloaded.")}</p>` : ""}${b.convert ? `<p class="v-lhint">${L("书库里只有 MOBI，设备读不了。点下面让书斋先转成 EPUB，转好自动下载。", "Only MOBI here; the library converts it first.")}</p>` : ""}
      <div class="v-lacts">${main}${wish}</div></div>`;
}


const SHELF_STYLES = [
  { id: "gallery", label: "画廊", ref: "dpReader · 纸感留白", desc: "纯白纸面留白 + 书页层叠边缘，封面底部内嵌 2px 墨迹进度线，下方只留书名与百分比" },
  { id: "slash",   label: "斜切", ref: "Valet · 标志斜切", desc: "顶部「接着读」斜切主卡（呼应首页 11.2° 视觉语言），下方三列封面网格" },
  { id: "rail",    label: "层板", ref: "kiikoread · 极简托轨", desc: "封面底部立在 3px 双线墨色层板与网点阴影上，书名干净印在层板下方" },
  { id: "cards",   label: "目录", ref: "KOReader · 瑞士列表", desc: "无冗余外框，1.2px 墨线分隔；左侧缩略封面，右侧书名、来源、68px 精确进度条与大号Avenir百分比" },
];

function renderShelfGrid(all, style) {
  let list = all;
  let heroHtml = "";
  if (style === "slash" && all.length > 0) {
    const cur = all[0];
    const pct = Math.round((cur.progress || 0) * 100);
    const srcName = cur.src ? cur.src : "本机";
    heroHtml = `<button class="v-shero" ${cur.attr}>
      ${libCover(cur, " sm")}
      <span class="v-shero-main">
        <b>${esc(cur.title)}</b>
        <em>${esc(cur.author)} · ${esc(srcName)} · ${pct > 0 ? `已读 ${pct}%` : "未读"}</em>
        <i class="v-track"><i style="width:${pct}%"></i></i>
      </span>
      <span class="v-wedge">${L("接着读", "Read")}${ic("go")}</span>
    </button>`;
    list = all.slice(1);
  }
  const rows = [];
  for (let i = 0; i < list.length; i += 3) rows.push(list.slice(i, i + 3));
  const tiers = rows.map((row) => `
    <div class="v-stier is-${style}">
      <div class="v-sgrid">
        ${row.map((b) => {
          const pct = Math.round((b.progress || 0) * 100);
          const floatProg = !b.away && pct > 0
            ? `<i class="v-cov-fprog"><i><b style="width:${pct}%"></b></i></i>`
            : b.away
              ? `<i class="v-cov-faway">${esc(b.note)}</i>`
              : "";
          return `<button class="v-sitem" ${b.attr}>
            <div class="v-scov-box">${libCover(b)}${floatProg}</div>
            <span class="v-sname">${esc(b.title)}</span>
          </button>`;
        }).join("")}
      </div>
      ${style === "rail" ? `<div class="v-sledge"></div>` : ""}
    </div>`).join("");
  return `${heroHtml}<div class="v-sstage is-${style}">${tiers}</div>`;
}

function renderShelfCards(all) {
  return `<div class="v-lrows is-clean">${all.map((b) => {
    const pct = Math.round((b.progress || 0) * 100);
    const srcText = b.src ? `${b.src}` : "本机";
    const rightCol = b.away
      ? `<small class="v-lstat is-away">${esc(b.note)}</small>`
      : `<span class="v-lpct">${pct > 0 ? `${pct}%` : `<small class="v-lstat">未读</small>`}</span>`;
    return `<button class="v-lrow-clean${b.away ? " away" : ""}" ${b.attr}>
      ${libCover(b, " sm")}
      <span class="v-lrow-main">
        <b>${esc(b.title)}</b>
        <em>${esc(b.author)} · ${esc(srcText)} · ${esc(b.fmt)}</em>
        ${b.away ? "" : `<i class="v-track"><i style="width:${pct}%"></i></i>`}
      </span>
      ${rightCol}
    </button>`;
  }).join("")}</div>`;
}

function v2Shelf() {
  if (state.libOpds) return v2OpdsScreen();
  if (state.libBook) {
    const b = LIB_BOOKS.find((x) => x.id === state.libBook);
    return `${bar()}<main class="v-page">${v2Head(esc(b.title), LIB_SOURCES[b.lib].short)}<div class="v-pane v-lib">${libDetail(b)}</div></main>`;
  }
  const q = state.libQuery;
  const all = shelfItems(q);
  const count = q.trim() ? L(`找到 ${all.length} 本`, `${all.length} found`) : L(`${all.length} 本`, `${all.length} books`);
  const libBtn = state.libs.length ? `<button class="v-ltag-btn" data-opds="1">${L("书库", "Libraries")}</button>` : "";
  const view = `<div class="seg tiny v-lview"><button data-shelf="cover" class="${state.shelfView === "cover" ? "on" : ""}">${t("covers")}</button><button data-shelf="list" class="${state.shelfView === "list" ? "on" : ""}">${t("list")}</button></div>`;
  const search = `<label class="v-lsearch">${ic("search")}<input data-lib-q value="${esc(q)}" placeholder="${L("书名、作者或拼音首字母", "Title or author")}">${q ? `<button data-lib-clear>${L("清空", "Clear")}</button>` : ""}</label>`;
  const body = !all.length ? `<p class="v-lhint">${L("没找到。换个词，或者只打拼音首字母试试。", "Nothing. Try other words.")}</p>`
    : state.shelfView === "list"
      ? renderShelfCards(all)
      : renderShelfGrid(all, "gallery");
  const none = state.libs.length ? "" : `<button class="v-line dashed" data-setting="library">${L("订一个书库，书库里的书也会出现在这里", "Subscribe to a library to see its books here")}</button>`;
  const rightAct = `<div class="v-lhead-acts">${libBtn}${view}</div>`;
  return `${bar()}<main class="v-page">${v2Head(L("书架", "Shelf"), count, rightAct)}<div class="v-pane v-lib">${search}${body}${none}</div></main>`;
}

// ── 书库浏览页（对应固件 kScreenOpds）──────────────────────────────────────
const OPDS_NAV = [
  { title: L("字母排序书籍", "By title"), nav: "letter" },
  { title: L("热门书籍", "Popular"), nav: "hot" },
  { title: L("最高评分书籍", "Top rated"), nav: "rated" },
  { title: L("最近添加的书籍", "Recently added"), nav: "new" },
  { title: L("随机书籍", "Random"), nav: "discover" },
  { title: L("已读书籍", "Read"), nav: "readbooks" },
  { title: L("未读书籍", "Unread"), nav: "unreadbooks" },
  { title: L("作者", "Authors"), nav: "author" },
  { title: L("出版社", "Publishers"), nav: "publisher" },
  { title: L("分类", "Categories"), nav: "category" },
  { title: L("丛书", "Series"), nav: "series" },
  { title: L("语言", "Languages"), nav: "language" },
  { title: L("书架列表", "Shelves"), nav: "shelfindex" },
];
const OPDS_BOOKS = LIB_BOOKS.filter((b) => !b.via);

function opdsEntries(nav) {
  if (!nav) return OPDS_NAV.map((e) => ({ title: e.title, nav: e.nav, files: [] }));
  if (nav === "new" || nav === "hot" || nav === "rated" || nav === "readbooks" || nav === "unreadbooks" || nav === "discover")
    return OPDS_BOOKS.map((b) => ({ title: b.title, author: b.author, fmt: b.fmt, size: b.size, id: b.id, files: [{}] }));
  if (nav === "author") return [...new Set(OPDS_BOOKS.map((b) => b.author))].map((a) => ({ title: a, nav: "author:" + a, files: [] }));
  if (nav.startsWith("author:")) return OPDS_BOOKS.filter((b) => b.author === nav.slice(7)).map((b) => ({ title: b.title, author: b.author, fmt: b.fmt, size: b.size, id: b.id, files: [{}] }));
  return OPDS_BOOKS.map((b) => ({ title: b.title, author: b.author, fmt: b.fmt, size: b.size, id: b.id, files: [{}] }));
}

function opdsNavTitle(nav) {
  if (!nav) return L("书库", "Library");
  const e = OPDS_NAV.find((x) => x.nav === nav);
  if (e) return e.title;
  if (nav.startsWith("author:")) return nav.slice(7);
  if (nav === "search") return L(`搜「${state.libOpdsQuery}」`, `Search "${state.libOpdsQuery}"`);
  return nav;
}

function opdsSearchEntries(q) {
  const k = q.trim().toLowerCase();
  if (!k) return [];
  return OPDS_BOOKS.filter((b) => b.title.toLowerCase().includes(k) || b.author.toLowerCase().includes(k) || (b.py && b.py.includes(k)))
    .map((b) => ({ title: b.title, author: b.author, fmt: b.fmt, size: b.size, id: b.id, files: [{}] }));
}

function v2OpdsScreen() {
  const nav = state.libOpdsBrowse;
  const title = opdsNavTitle(nav);
  const entries = nav === "search" ? opdsSearchEntries(state.libOpdsQuery) : opdsEntries(nav);
  const rows = entries.map((e, i) => {
    const book = e.files && e.files.length;
    const lib = e.id ? LIB_BOOKS.find((b) => b.id === e.id) : null;
    const got = lib && state.libGot[lib.id] === true;
    const tail = !book ? "›" : got ? L("在本机", "Here") : "";
    const meta = book ? `${e.author ? esc(e.author) + " · " : ""}${e.fmt || ""} · ${e.size || ""}` : "";
    return `<button class="v-opds-row" data-opds-entry="${i}"><span><b>${esc(e.title)}</b>${meta ? `<em>${meta}</em>` : ""}</span>${tail ? `<small>${tail}</small>` : ""}</button>`;
  });
  const q = state.libOpdsQuery;
  const searchBtn = `<button class="v-ltag-btn" data-opds-search="1">${L("搜书", "Search")}</button>`;
  const searchBar = `<div class="v-lhead-row"><b>${esc(title)}</b>${searchBtn}</div>`;
  const body = rows.length ? `<div class="v-opds-list">${rows.join("")}</div>`
    : `<p class="v-lhint">${L("正在连书库…", "Connecting…")}</p>`;
  const pg = entries.length ? `<div class="v-lbar"><span>${L(`${entries.length} 项`, `${entries.length} items`)}</span></div>` : "";
  return `${bar()}<main class="v-page">${v2Head("")}<div class="v-pane v-lib">${searchBar}${pg}${body}</div></main>`;
}
function libSettingsPane() {
  const rows = libOn().map((s) => `<div class="v-row"><span><b>${esc(s.name)}</b><em>${esc(new URL(s.url).host)}${s.rich ? " · 杜万" : ""}</em></span>
      <small class="v-lok">${s.rich ? L("带进度", "Syncs progress") : "OPDS"}</small><button class="v-line" data-lib-remove="${s.id}">${L("移除", "Remove")}</button></div>`).join("");
  const add = state.libAdding
    ? `<div class="v-fld"><label>${L("订阅链接", "Link")}</label><div class="v-inputs one"><input data-lib-url placeholder="https://…/opds/…"></div>
        <div class="v-two"><button class="v-line" data-lib-add-cancel>${L("取消", "Cancel")}</button><button class="v-solid" data-lib-add-save>${L("订阅", "Subscribe")}</button></div></div>`
    : `<div class="v-two"><button class="v-line" data-lib-phone>${L("在手机上填", "From your phone")}</button><button class="v-line" data-lib-add>${L("在这里打", "Type it here")}</button></div>
      <p class="v-tip">${L("链接很长，用手机或电脑粘贴更省事：同一个 Wi-Fi 下扫码打开配置页，在「书库订阅」里粘贴。", "Long links are easier to paste from a phone.")}</p>`;
  const fake = state.libs.includes("gutenberg") ? "" : `<button class="v-line dashed" data-lib-fake="gutenberg">${L("假的：再订一个古登堡（看多个书库）", "Fake: add Gutenberg")}</button>`;
  return `${rows ? `<nav class="v-rows">${rows}</nav>` : `<div class="v-card bad">${ic("x")}<span><b>${L("还没有订阅书库", "No library yet")}</b><em>${L("订了以后，书库里的书和本机的书放在一个书架上", "Subscribed books join the shelf")}</em></span></div>`}
    <p class="v-tip">${L("填 OPDS 链接，可以订好几个。书库的书和本机的书放在一个书架上，封面角上斜贴着来自哪个书库；搜书时一起搜。书斋这类书库还会同步在读、想读和读到哪里。", "Add OPDS links. Their books join the shelf, tagged on the corner.")}</p>
    ${add}${fake}`;
}

function libDownload(id) {
  const b = LIB_BOOKS.find((x) => x.id === id);
  const start = () => {
    state.libGot[id] = 0;
    const step = () => {
      if (typeof state.libGot[id] !== "number") return;
      state.libGot[id] = Math.min(1, state.libGot[id] + 0.25);
      if (state.libGot[id] >= 1) { state.libGot[id] = true; toast(L(`《${b.title}》下好了`, "Downloaded")); log(`下载完：《${b.title}》存到 SD 卡 /books/，封面一起下了。`); return; }
      render(); later(`lib-${id}`, 450, step);
    };
    render(); later(`lib-${id}`, 450, step);
  };
  if (b.via) {
    state.libTake[id] = true;
    log(`《${b.title}》书斋里还没有：POST take_url，书斋从${LIB_SOURCES[b.lib].name}收进书库，设备每 2 秒问一次 job，done 后拿 download_url（真书斋约 6 秒）。`);
    render();
    later(`libt-${id}`, 2000, () => { delete state.libTake[id]; b.lib = "shuzhai"; b.via = undefined; b.status = "unread"; start(); });
  } else if (b.convert) {
    state.libConvert[id] = true;
    log(`《${b.title}》只有 MOBI：请书斋转成 EPUB，转好再下（真书斋约 5 秒）。`);
    render();
    later(`libc-${id}`, 1600, () => { delete state.libConvert[id]; b.convert = false; b.fmt = "EPUB"; b.size = "3.8 MB"; start(); });
  } else start();
}

panel.addEventListener("input", (e) => {
  if (!isV2() || !e.target.matches("[data-lib-q]")) return;
  state.libQuery = e.target.value;
  const pos = e.target.selectionStart;
  render();
  const box = panel.querySelector("[data-lib-q]");
  if (box) { box.focus(); box.setSelectionRange(pos, pos); }
});

panel.addEventListener("click", (e) => {
  if (!isV2() || blocked()) return;
  const hit = (sel) => e.target.closest(sel);
  let el;
  if (hit("[data-lib-clear]")) { state.libQuery = ""; return render(); }
  if ((el = hit("[data-shelf-style]"))) {
    state.shelfStyle = el.dataset.shelfStyle;
    const s = SHELF_STYLES.find((x) => x.id === state.shelfStyle);
    if (s) log(`切换书架风格为「${s.label}」（${s.ref}）：${s.desc}。`);
    flash();
    return render();
  }
  if ((el = hit("[data-cover-tpl]"))) {
    state.coverTpl = el.dataset.coverTpl;
    log(`切换无图封面模板为「${state.coverTpl}」。`);
    flash();
    return render();
  }
  // 书架 → 书库浏览页
  if (hit("[data-opds]")) { state.libOpds = true; state.libOpdsBrowse = null; state.libOpdsQuery = ""; flash(); return render(); }
  // 书库浏览页内部
  if (hit("[data-opds-search]")) {
    const q = window.prompt(L("搜书（书名/作者/拼音首字母）", "Search (title/author)")) || "";
    if (!q.trim()) return;
    state.libOpdsQuery = q.trim();
    state.libOpdsBrowse = "search";
    flash();
    return render();
  }
  if ((el = hit("[data-opds-entry]"))) {
    const idx = Number(el.dataset.opdsEntry);
    const entries = state.libOpdsBrowse === "search" ? opdsSearchEntries(state.libOpdsQuery) : opdsEntries(state.libOpdsBrowse);
    const e = entries[idx];
    if (!e) return;
    if (e.files && e.files.length && e.id) {
      const b = LIB_BOOKS.find((x) => x.id === e.id);
      if (b) { state.libOpds = false; state.libBook = b.id; flash(); return render(); }
    } else if (e.nav) {
      state.libOpdsBrowse = e.nav;
      flash();
      return render();
    }
    return;
  }
  if ((el = hit("[data-lib-book]"))) { state.libBook = Number(el.dataset.libBook); flash(); return render(); }
  if ((el = hit("[data-lib-get]"))) return libDownload(Number(el.dataset.libGet));
  if ((el = hit("[data-lib-wish]"))) {
    const b = LIB_BOOKS.find((x) => x.id === Number(el.dataset.libWish));
    b.wish = !b.wish;
    log(`${b.wish ? "加到" : "移出"}想读：书斋网页上同时变。`);
    return render();
  }
  if ((el = hit("[data-lib-read]"))) {
    const b = LIB_BOOKS.find((x) => x.id === Number(el.dataset.libRead));
    log(`打开《${b.title}》：先问书斋读到哪里${b.progress ? `（${Math.round(b.progress * 100)}%）` : ""}，比本机新就跳过去。假壳子里打开的是三国演义的正文。`);
    state.libBook = null;
    return go("reader");
  }
  if ((el = hit("[data-lib-fake]"))) {
    const id = el.dataset.libFake;
    if (!state.libs.includes(id)) state.libs.push(id);
    log(`假的：订上「${LIB_SOURCES[id].name}」。`);
    return render();
  }
  if ((el = hit("[data-lib-remove]"))) {
    state.libs = state.libs.filter((x) => x !== el.dataset.libRemove);
    SetupPage.repaintLibraries();
    return render();
  }
  if (hit("[data-lib-phone]")) {
    log("书库订阅：设备显示配置页的二维码（和添加助理是同一个页面），手机打开后直接到「书库订阅」。");
    SetupPage.open("library");
    return render();
  }
  if (hit("[data-lib-add]")) { state.libAdding = true; render(); const i = panel.querySelector("[data-lib-url]"); if (i) i.focus(); return; }
  if (hit("[data-lib-add-cancel]")) { state.libAdding = false; return render(); }
  if (hit("[data-lib-add-save]")) {
    const v = (panel.querySelector("[data-lib-url]") || {}).value || "";
    state.libAdding = false;
    if (!/^https?:\/\/\S+$/i.test(v.trim())) { toast(L("要填 http 或 https 开头的链接", "Needs an http(s) link")); return; }
    const id = v.includes("gutenberg") ? "gutenberg" : "shuzhai";
    if (!state.libs.includes(id)) state.libs.push(id);
    toast(L(`已订阅「${LIB_SOURCES[id].short}」`, "Subscribed"));
    return;
  }
});
