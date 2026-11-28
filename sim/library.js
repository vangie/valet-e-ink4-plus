// 新设计 · 书库（OPDS 订阅）。书架不分栏：SD 卡上的书和订阅书库里的书放在一起，
// 来自书库的书在封面右上角斜着贴一条来源（「微读」「书库」「古登堡」），没贴的就是本机自己的书。
// 书库的书没下载时封面是虚线框，下面写「未下载」。书架上只放书库的在读、想读和最近加入，其余的靠搜。
// 订阅可以有多个（别的用户可能订好几个 OPDS），自建书库这类还带「在读 / 想读」和阅读进度。
// 设备键盘打不了中文，所以搜书时拼音首字母也算（hlm → 红楼梦），英文书名照常打。
// 自建书库支持把上游书源（维基文库、古登堡）的结果放进同一个搜索：搜到还没收的书，书角标上游的名字，
// 点下载时先把书收进书库（take → 轮询 job，约 6 秒），再照常下载、同步进度。只在搜书时出现。
// 默认只内置微信读书；用户在「书库」里手动配置自己的 OPDS 书库或公网书库。

const LIB_BOOKS = [
  { id: 101, lib: "weread", title: "风景与权力", author: "W.J.T.米切尔", py: "fjyql", fmt: "EPUB", size: "4.8 MB", status: "reading", progress: 0.42, wish: false, recent: true,
    desc: "从文化景观与视觉权力的角度审视风景绘画与帝国记忆。" },
  { id: 3, lib: "custom", title: "围城", author: "钱钟书", py: "wc qianzhongshu", fmt: "EPUB", size: "1.2 MB", status: "reading", progress: 0.35, wish: false, recent: true,
    desc: "方鸿渐留学归来，在婚姻和职业里进进出出。" },
  { id: 102, lib: "weread", title: "三体", author: "刘慈欣", py: "st liucixin", fmt: "EPUB", size: "2.4 MB", status: "reading", progress: 0.68, wish: false, recent: true,
    desc: "红岸基地向宇宙发出的第一声啼鸣，引来了四光年外的三体舰队。" },
  { id: 7, lib: "custom", title: "长安的荔枝", author: "马伯庸", py: "cadlz mabaiyong", fmt: "EPUB", size: "860 KB", status: "reading", progress: 0.18, wish: true, recent: true,
    desc: "唐天宝十四年，上林署监事李善德接到一桩任务：在贵妃诞日之前，从岭南运来新鲜荔枝。" },
  { id: 103, lib: "weread", title: "置身事内", author: "兰小欢", py: "zssn lanxiaohuan", fmt: "EPUB", size: "1.5 MB", status: "unread", progress: 0, wish: true, recent: true,
    desc: "中国政府与经济发展：地方政府如何推动土地财政与产业投资。" },
  { id: 9, lib: "custom", title: "活着", author: "余华", py: "hz yuhua", fmt: "EPUB", size: "420 KB", status: "finished", progress: 1, wish: false, recent: false,
    desc: "地主少爷福贵的一生。" },
  { id: 12, lib: "custom", title: "小王子", author: "圣埃克苏佩里", py: "xwz", fmt: "EPUB", size: "1.6 MB", status: "unread", progress: 0, wish: true, recent: true,
    desc: "一个住在小行星上的王子，离开他的玫瑰去旅行。" },
  { id: 15, lib: "custom", title: "明朝那些事儿", author: "当年明月", py: "mcnxse dangnianmingyue", fmt: "MOBI", size: "5.1 MB", status: "unread", progress: 0, wish: false, recent: true, convert: true,
    desc: "从朱元璋出生讲到明朝灭亡，讲人，也讲制度。" },
  { id: 18, lib: "custom", title: "人间词话", author: "王国维", py: "rjch wangguowei", fmt: "EPUB", size: "340 KB", status: "reading", progress: 0.08, wish: false, recent: false,
    desc: "以「境界」论词，从李白、温庭筠一路讲到纳兰性德。" },
  { id: 9001, lib: "wikisource", via: "custom", title: "The Art of War", author: "Sun Tzu", py: "", fmt: "EPUB", size: "210 KB",
    desc: "Lionel Giles 1910 年的英译本，十三篇。" },
  { id: 9002, lib: "wikisource", via: "custom", title: "The Analects", author: "Confucius", py: "", fmt: "EPUB", size: "380 KB",
    desc: "James Legge 的英译本。" },
  { id: 2701, lib: "gutenberg", title: "Moby Dick", author: "Herman Melville", py: "", fmt: "EPUB", size: "1.3 MB", recent: true,
    desc: "Ishmael's voyage on the whaler Pequod, commanded by Captain Ahab." },
  { id: 1342, lib: "gutenberg", title: "Pride and Prejudice", author: "Jane Austen", py: "", fmt: "EPUB", size: "780 KB", recent: true,
    desc: "Elizabeth Bennet and Mr Darcy." },
];
const LIB_SOURCES = {
  weread: { id: "weread", name: "微信读书", short: "微读", url: "https://weread.qq.com", rich: true },
  custom: { id: "custom", name: "自建书库", short: "书库", url: "https://books.example.com/opds", rich: true, auth: true },
  gutenberg: { id: "gutenberg", name: "古登堡", short: "古登堡", url: "https://www.gutenberg.org/ebooks.opds/", rich: false },
  wikisource: { id: "wikisource", name: "维基文库（英文）", short: "维基文库", upstream: true },
};
Object.assign(state, {
  libs: ["weread"], // 默认不内置私有书库，仅内置微信读书；用户可手动添加自己的书库
  libQuery: "",
  libBook: null,          // 正在看详情的那本
  libGot: { 101: true, 3: true, 102: true, 7: true }, // 已下载到本机：id → true；正在下：id → 0..1
  libConvert: {},         // 正在转 EPUB：id → true
  libTake: {},            // 正在从上游收这本：id → true
  libAdding: false,       // 设置里正在填新订阅
  libOpds: false,         // 是否在看书库配置/浏览页（固件 kScreenOpds）
  libOpdsBrowse: null,    // 当前书库浏览层：{title, entries:[{title,nav,files,size}], page}
  libOpdsQuery: "",       // 书库搜索框里的词
  wereadLoggedIn: false,  // 微信读书是否已扫码登录
  wereadQrOpen: false,    // 微信读书扫码弹窗是否打开
});

function libOn() { return state.libs.map((id) => LIB_SOURCES[id]).filter(Boolean); }
// 上游的书挂在自建书库名下：订了自建书库才有，而且只在搜书时出现。
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
  if (state.libConvert[b.id]) return L("书库在转 EPUB…", "Converting…");
  if (state.libTake[b.id]) return L("书库在收录…", "Adding…");
  if (b.via) return L("书库可收", "Can add");
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
  if (b.srcId === "custom" || b.src === "书库") return " is-paper";
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
  else if (state.libTake[b.id]) main = `<button class="v-solid" disabled>${L("书库在收录…", "Adding to the library…")}</button>`;
  else if (b.via) main = `<button class="v-solid" data-lib-get="${b.id}">${L("收进书库，再下载", "Add to library, then download")}</button>`;
  else if (state.libConvert[b.id]) main = `<button class="v-solid" disabled>${L("书库在转 EPUB…", "Converting…")}</button>`;
  else if (b.convert) main = `<button class="v-solid" data-lib-get="${b.id}">${L("转成 EPUB 再下载", "Convert, then download")}</button>`;
  else main = `<button class="v-solid" data-lib-get="${b.id}">${L("下载到书架", "Download")} · ${b.size}</button>`;
  const wish = src.rich && !b.via && b.status !== "finished"
    ? `<button class="v-line${b.wish ? " on" : ""}" data-lib-wish="${b.id}">${b.wish ? L("已在想读 · 取消", "In list · remove") : L("加到想读", "Want to read")}</button>` : "";
  const prog = src.rich && b.progress ? `<p class="v-lhint">${L(`读到 ${Math.round(b.progress * 100)}%，在这台设备或网页上读都会接着。`, "Progress syncs with your other devices.")}</p>` : "";
  return `<div class="v-lbook">
      <div class="v-lbook-top">${libCover(b, " big")}<div><h3>${esc(b.title)}</h3><p>${esc(b.author)}</p>
        <p class="v-lmeta">${L("来自", "From")}「${esc(src.short)}」${b.via ? L("，经自建书库", " via library") : ""} · ${b.fmt} · ${b.size}</p>${libStatus(b) ? `<p class="v-lmeta"><b>${libStatus(b)}</b></p>` : ""}</div></div>
      <p class="v-ldesc">${esc(b.desc)}</p>${prog}${b.via ? `<p class="v-lhint">${L("书库里还没有这本。点下面从上游书源收进书库，收好自动下载；以后进度照常同步。", "Not in library yet. It is added first, then downloaded.")}</p>` : ""}${b.convert ? `<p class="v-lhint">${L("书库里只有 MOBI，设备读不了。点下面让书库先转成 EPUB，转好自动下载。", "Only MOBI here; the library converts it first.")}</p>` : ""}
      <div class="v-lacts">${main}${wish}</div></div>`;
}


const SHELF_STYLES = [
  { id: "gallery", label: "画廊", ref: "dpReader · 纸感留白", desc: "纯白纸面留白 + 书页层叠边缘，封面底部内嵌 2px 墨迹进度线，下方只留书名与百分比" },
  { id: "slash",   label: "斜切", ref: "Valet · 标志斜切", desc: "顶部「接着读」斜切主卡（呼应首页 11.2° 视觉语言），下方三列封面网格" },
  { id: "rail",    label: "层板", ref: "kiikoread · 极简托轨", desc: "封面底部立在 3px 双线墨色层板与网点阴影上，书名干净印在层板下方" },
  { id: "cards",   label: "目录", ref: "KOReader · 瑞士列表", desc: "无冗余外框，1.2px 墨线分隔；左侧缩略封面，右侧书名、来源、68px 精确进度条与大号Avenir百分比" },
];

const SHELF_LIST_STYLES = [
  { id: "twoline",   label: "双行主次", desc: "无封面，一屏 7 本。上行粗体书名（去后缀），下行「作者 · 来源」，右侧大号 Avenir 百分比 + 40px 迷你进度条" },
  { id: "bottombar", label: "底轨进度", desc: "无封面，一屏 7 本。行底分隔线本身加厚为通栏进度条（呼应封面底框墨线），左侧书名+作者，右侧「来源标签 + 百分比」" },
  { id: "compact",   label: "单行紧凑", desc: "无封面，一屏 9 本高密度。单行左书名、中作者、右来源小标与百分比，适合藏书多时快速扫视" },
  { id: "catalog",   label: "左竖标线", desc: "无封面，一屏 7 本。左侧 3px 竖线代表在读状态（实心=在读，虚线=未下，浅灰=未读），中间书名+作者·来源，右侧进度" },
];

function renderShelfGrid(all, style) {
  const rows = [];
  for (let i = 0; i < all.length; i += 3) rows.push(all.slice(i, i + 3));
  const tiers = rows.map((row) => `
    <div class="v-stier is-${style} pm-edge">
      <div class="v-sgrid">
        ${row.map((b) => {
          const pct = Math.round((b.progress || 0) * 100);
          const covOverlay = !b.away && pct > 0 ? `<i class="v-prog-edge"><b style="width:${pct}%"></b></i>` : "";
          return `<button class="v-sitem" ${b.attr}>
            <div class="v-scov-box">${libCover(b)}${covOverlay}</div>
            <span class="v-sname">${esc(b.title)}</span>
          </button>`;
        }).join("")}
      </div>
    </div>`).join("");
  return `<div class="v-sstage is-${style}">${tiers}</div>`;
}

function renderShelfList(all) {
  const ls = state.shelfListStyle || "twoline";
  return `<div class="v-llist is-${ls}">${all.map((b) => {
    const pct = Math.round((b.progress || 0) * 100);
    const srcName = b.src || "";
    const authorText = b.author || "佚名";
    const badge = srcName ? `<i class="v-lsrc-pill${b.srcId === "weread" ? " is-dark" : ""}">${esc(srcName)}</i>` : "";

    if (ls === "twoline") {
      // 方案 1：双行主次（左侧书名 + 作者·来源，右侧大百分比 + 迷你进度条）
      const meta = [authorText, srcName || "本机"].filter(Boolean).join(" · ");
      const right = b.away
        ? `<span class="v-lr-box"><small class="v-lstat is-away">${esc(b.note)}</small></span>`
        : pct > 0
          ? `<span class="v-lr-box"><b class="v-lpct">${pct}%</b><i class="v-lmini-bar"><b style="width:${pct}%"></b></i></span>`
          : `<span class="v-lr-box"><small class="v-lstat">未读</small></span>`;
      return `<button class="v-litem is-twoline${b.away ? " away" : ""}" ${b.attr}>
        <span class="v-ll-main">
          <b>${esc(b.title)}</b>
          <em>${esc(meta)}</em>
        </span>
        ${right}
      </button>`;
    }

    if (ls === "bottombar") {
      // 方案 2：底轨墨线（行底线本身就是通栏进度轨，呼应封面底框墨线）
      const rightTxt = b.away ? `<small class="v-lstat is-away">${esc(b.note)}</small>`
        : pct > 0 ? `<b class="v-lpct">${pct}%</b>` : `<small class="v-lstat">未读</small>`;
      const edgeBar = !b.away && pct > 0 ? `<i class="v-lrow-edge"><b style="width:${pct}%"></b></i>` : "";
      return `<button class="v-litem is-bottombar${b.away ? " away" : ""}" ${b.attr}>
        <span class="v-ll-main">
          <b>${esc(b.title)}</b>
          <em>${esc(authorText)}</em>
        </span>
        <span class="v-lr-inline">${badge}${rightTxt}</span>
        ${edgeBar}
      </button>`;
    }

    if (ls === "compact") {
      // 方案 3：单行高密度紧凑列表（一屏 9 本，书名 · 作者 — 来源 · 进度）
      const rightTxt = b.away ? `<small class="v-lc-stat is-away">未下</small>`
        : pct > 0 ? `<b class="v-lc-pct">${pct}%</b>` : `<small class="v-lc-stat">—</small>`;
      return `<button class="v-litem is-compact${b.away ? " away" : ""}" ${b.attr}>
        <b class="v-lc-title">${esc(b.title)}</b>
        <em class="v-lc-author">${esc(authorText)}</em>
        ${badge}
        ${rightTxt}
      </button>`;
    }

    // 方案 4：左竖标线（左侧 3px 状态竖条 + 书名/作者 + 右侧胶囊来源与进度）
    const meta = [authorText, srcName || "本机"].filter(Boolean).join(" · ");
    const barState = b.away ? "is-away" : pct > 0 ? "is-reading" : "is-unread";
    const right = b.away
      ? `<small class="v-lstat is-away">${esc(b.note)}</small>`
      : pct > 0
        ? `<span class="v-lcat-r"><b>${pct}%</b><i class="v-lmini-bar"><b style="width:${pct}%"></b></i></span>`
        : `<small class="v-lstat">未读</small>`;
    return `<button class="v-litem is-catalog${b.away ? " away" : ""}" ${b.attr}>
      <i class="v-lcat-bar ${barState}"><b style="height:${b.away ? 0 : pct}%"></b></i>
      <span class="v-ll-main">
        <b>${esc(b.title)}</b>
        <em>${esc(meta)}</em>
      </span>
      ${right}
    </button>`;
  }).join("")}</div>`;
}

const SIM_PY_PHRASES = {
  santi: "三体", sanguo: "三国演义", honglou: "红楼梦", hongloumeng: "红楼梦", xiyou: "西游记", xiyouji: "西游记",
  shuihu: "水浒传", shuihuzhuan: "水浒传", huozhe: "活着", bailuyuan: "白鹿原", baiyexing: "白夜行",
  weicheng: "围城", pingfan: "平凡的世界", bainian: "百年孤独", lishi: "历史", zhexue: "哲学",
  jingji: "经济", xinli: "心理学", kehuan: "科幻", xiaoshuo: "小说", wenxue: "文学",
  luxun: "鲁迅", jinyong: "金庸", yuhua: "余华", moyan: "莫言", liucixin: "刘慈欣",
  dongye: "东野圭吾", cunchun: "村上春树", gongqijun: "宫崎骏", wangxiaobo: "王小波",
};
const SIM_PY_ASSOC = {
  "三": "体国毛联生十年", "红": "楼梦高粱与黑色", "楼": "梦", "西": "游方洋汉", "游": "记戏",
  "水": "浒平果墨", "浒": "传", "白": "鹿夜银居马", "鹿": "原鼎记", "夜": "行晚色",
  "百": "年科度家", "年": "代孤独轻", "孤": "独儿", "平": "凡安常静", "凡": "的",
  "人": "生类民物间工", "生": "命活物死", "围": "城棋", "朝": "花夕拾代", "天": "龙下空地才",
  "龙": "八部族", "射": "雕击", "神": "雕曲秘话", "笑": "傲江湖话", "雪": "山飞狐花",
  "明": "朝代天白", "万": "历水里年", "乡": "土村愁", "置": "身事内", "文": "化城学明章",
  "沉": "默思", "黄": "金河帝色", "时": "间代候光", "未": "来知", "中": "国医华央",
  "哲": "学理思", "历": "史代程", "政": "治府策", "思": "考想维", "自": "私由然己律",
  "理": "想国论解财", "国": "富论家学际", "道": "德经家理路", "论": "语坛文", "资": "治通鉴本金",
  "解": "忧决放释", "嫌": "疑人", "挪": "威的森林", "小": "王子说学", "月": "亮与六便士光",
  "悲": "惨世界剧", "战": "争与和平略", "老": "人与海舍子师", "黑": "暗森林天鹅铁",
  "流": "浪地球星水", "失": "控败去", "精": "进神彩", "反": "脆弱思", "鲁": "迅",
  "金": "庸瓶梅融钱", "古": "龙代典文", "余": "华生", "莫": "言", "刘": "慈欣备",
  "东": "野圭吾方周坡", "野": "圭吾草", "圭": "吾", "村": "上春树庄", "宫": "崎骏廷",
  "崎": "骏", "王": "小波阳明维国维", "陈": "忠实独秀", "贾": "平凹", "钱": "钟书",
  "张": "爱玲之洞", "沈": "从文", "史": "铁生记", "苏": "轼东坡童", "李": "白鸿章",
  "曹": "雪芹操", "罗": "贯中振宇辑", "司": "马迁马光", "经": "济学典历", "社": "会学区",
  "心": "理学态情", "管": "理学道", "人": "工智能生类", "计": "算机划", "小": "说王子",
};

function simGetKbCandidates() {
  const zh = state.kbZh !== false;
  const py = state.kbPy || "";
  const cands = [];
  if (!zh) return cands;
  if (py) {
    if (SIM_PY_PHRASES[py]) cands.push(SIM_PY_PHRASES[py]);
    if (py.length >= 4) {
      for (const [k, v] of Object.entries(SIM_PY_PHRASES)) {
        if (k !== py && k.startsWith(py) && !cands.includes(v)) cands.push(v);
      }
    }
    const exact = SIM_PY_DICT[py] || "";
    for (const ch of exact) if (!cands.includes(ch)) cands.push(ch);
    for (const [k, v] of Object.entries(SIM_PY_DICT)) {
      if (k !== py && k.startsWith(py)) {
        for (const ch of v.slice(0, 4)) if (!cands.includes(ch)) cands.push(ch);
      }
    }
  } else if (state.kbAssoc) {
    for (const ch of (SIM_PY_ASSOC[state.kbAssoc] || "")) cands.push(ch);
  }
  return cands;
}

function renderSearchKbOverlay() {
  if (!state.kbOpen) return "";
  const zh = state.kbZh !== false;
  const py = state.kbPy || "";
  const page = state.kbCandPage || 0;
  const cands = simGetKbCandidates();
  const slots = cands.slice(page * 7, page * 7 + 7);
  const barTitle = py ? py : (state.kbAssoc && cands.length ? "联想" : "拼音");
  const candBar = zh ? `<div style="display:flex;align-items:center;border:1px solid #000;border-radius:4px;height:26px;padding:0 6px;margin-bottom:4px;font-size:11px;gap:4px">
    <span style="width:42px;font-weight:700;overflow:hidden">${esc(barTitle)}</span>
    <div style="flex:1;display:flex;justify-content:space-around">${slots.map((ch, i) => `<button data-kb-cand="${i}" style="border:0;background:none;font-size:12px;font-weight:700;cursor:pointer;padding:2px 4px">${ch}</button>`).join("")}</div>
    ${page > 0 ? `<button data-kb-page="-1" style="border:1px solid #000;border-radius:3px;background:#fff;font-size:10px;padding:0 4px;cursor:pointer">&lt;</button>` : ""}
    ${(page + 1) * 7 < cands.length ? `<button data-kb-page="1" style="border:1px solid #000;border-radius:3px;background:#fff;font-size:10px;padding:0 4px;cursor:pointer">&gt;</button>` : ""}
  </div>` : "";
  const row = (chars) => `<div style="display:flex;gap:3px;justify-content:center;margin-bottom:3px">${[...chars].map((c) => `<button data-kb-char="${c}" style="flex:1;height:26px;border:1px solid #000;border-radius:4px;background:#fff;font-size:11px;font-weight:600;cursor:pointer">${c}</button>`).join("")}</div>`;
  return `<div class="v-sheet-mask" style="position:absolute;inset:0;background:rgba(0,0,0,.35);display:flex;align-items:flex-end;z-index:25">
    <div style="width:100%;background:#fff;border-top:2px solid #000">
      <div style="background:#000;color:#fff;padding:6px 12px;display:flex;justify-content:space-between;align-items:center;font-size:12px">
        <b>${L("搜书", "Search")}</b>
        <button data-kb-close="1" style="color:#fff;text-decoration:underline;background:none;border:0;cursor:pointer;font-size:11px">${L("取消", "Cancel")}</button>
      </div>
      <div style="padding:8px 12px">
        <div style="font-size:10px;color:#333;margin-bottom:6px">${L("同时搜索微信读书与已配置书库，支持书名、作者或拼音首字母。", "Searches WeRead and every configured library.")}</div>
        <div style="border:1.5px solid #000;height:30px;display:flex;align-items:center;padding:0 8px;font-size:12px;font-weight:600;margin-bottom:6px">${esc(state.kbText || "") || `<span style="color:#666">${L("书名、作者", "Title or author")}</span>`}</div>
        ${candBar}
        ${row("qwertyuiop")}
        ${row("asdfghjkl")}
        <div style="display:flex;gap:3px;margin-bottom:3px">
          <button data-kb-shift="1" style="width:34px;height:26px;border:1px solid #000;border-radius:4px;background:#fff;font-size:11px;cursor:pointer">⇧</button>
          ${[..."zxcvbnm"].map((c) => `<button data-kb-char="${c}" style="flex:1;height:26px;border:1px solid #000;border-radius:4px;background:#fff;font-size:11px;font-weight:600;cursor:pointer">${c}</button>`).join("")}
          <button data-kb-bs="1" style="width:34px;height:26px;border:1px solid #000;border-radius:4px;background:#fff;font-size:11px;cursor:pointer">⌫</button>
        </div>
        <div style="display:flex;gap:3px">
          <button data-kb-sym="1" style="width:42px;height:26px;border:1px solid #000;border-radius:4px;background:#fff;font-size:10px;cursor:pointer">?123</button>
          <button data-kb-lang="1" style="width:46px;height:26px;border:${zh ? "2px" : "1px"} solid #000;border-radius:4px;background:#fff;font-size:10px;font-weight:700;cursor:pointer">${zh ? "中/英" : "英/中"}</button>
          <button data-kb-space="1" style="flex:1;height:26px;border:1px solid #000;border-radius:4px;background:#fff;font-size:10px;cursor:pointer">${zh ? "空格" : "Space"}</button>
          <button data-kb-ok="1" style="width:46px;height:26px;border:1.5px solid #000;border-radius:4px;background:#fff;font-size:12px;font-weight:700;cursor:pointer">✓</button>
        </div>
      </div>
    </div>
  </div>`;
}

const SIM_PY_DICT = {
  a: "阿啊", ai: "爱哀埃碍艾", an: "安案暗岸按", ba: "八巴吧把爸霸", bai: "百白拜败柏", ban: "半办版班般板",
  bao: "包宝保报抱暴薄", bei: "北被倍备悲杯碑背", ben: "本奔笨", bi: "比笔必彼币壁避闭", bian: "边变便编遍辨辩",
  biao: "表标彪", bie: "别憋", bin: "宾滨彬", bing: "并病兵冰丙", bo: "波伯博播薄玻璃", bu: "不步部布补捕",
  cai: "才采彩材菜财裁", can: "参残惨餐灿", cang: "藏苍仓沧舱", cao: "草曹操槽", ce: "策侧册测", ceng: "曾层",
  cha: "茶查察差插叉", chai: "拆柴", chan: "产单禅缠蝉", chang: "长场常唱厂偿畅", chao: "朝超潮吵抄", che: "车彻扯撤",
  chen: "陈沉尘晨臣辰", cheng: "成城程承乘称诚", chi: "吃持尺池迟赤翅", chong: "重充冲虫宠崇", chou: "愁抽仇丑筹绸",
  chu: "出初楚处除厨础触", chuan: "传船川穿串", chuang: "创窗床闯", chui: "吹垂锤", chun: "春纯唇醇", ci: "此词次辞慈刺磁",
  cong: "从聪丛匆", cu: "促粗簇", cun: "存村寸", cuo: "错措搓", da: "大打达答搭塔", dai: "代带待袋戴贷呆",
  dan: "但单担淡旦弹胆", dang: "当党档荡挡", dao: "到道岛导刀倒盗稻", de: "的得地德", deng: "等灯登邓",
  di: "地第底低帝弟敌滴", dian: "点电店典殿垫颠", diao: "调掉雕钓吊", die: "爹跌叠蝶", ding: "定顶丁钉订鼎",
  dong: "东动冬懂洞冻栋", dou: "都斗豆逗陡", du: "读度独都毒渡肚赌", duan: "段断短端锻", dui: "对队堆兑",
  dun: "盾顿蹲吨", duo: "多朵夺躲堕", e: "恶俄额鹅饿", en: "恩", er: "而二耳儿尔", fa: "发法罚乏伐阀",
  fan: "反饭翻凡犯范繁烦", fang: "方放房防访仿芳", fei: "非飞费肥废菲匪", fen: "分份纷粉愤坟奋", feng: "风封峰丰逢锋凤疯",
  fo: "佛", fu: "夫父服富福府复付负附浮妇肤", gai: "改该概盖钙", gan: "感干敢甘肝赶竿", gang: "刚港钢岗纲缸",
  gao: "高告搞稿糕膏", ge: "个各歌格哥革割阁葛", gei: "给", gen: "根跟", geng: "更耕庚颈", gong: "工公共功宫攻供恭巩",
  gou: "够构沟狗钩购", gu: "古故孤骨谷股鼓固姑", gua: "挂刮瓜寡卦", guai: "怪乖拐", guan: "关管官观馆惯冠贯罐",
  guang: "光广逛", gui: "归规鬼贵轨桂柜跪", gun: "滚棍", guo: "国过果锅裹郭", ha: "哈蛤", hai: "海还孩害亥",
  han: "汉寒含喊汗韩翰", hang: "行航杭巷", hao: "好号毫豪浩耗", he: "和合河何喝荷核贺鹤", hei: "黑嘿",
  hen: "很恨狠痕", heng: "横恒衡哼", hong: "红洪宏虹鸿哄", hou: "后候厚喉猴侯", hu: "乎呼湖护狐壶胡虎忽糊",
  hua: "话花化画华滑划哗", huai: "怀坏淮槐", huan: "还换欢环缓幻唤宦", huang: "黄皇荒慌煌晃谎", hui: "会回灰挥辉恢悔汇慧毁",
  hun: "混婚魂昏浑", huo: "活火或获惑祸货伙", ji: "记己几机及集级急计际极既纪技季寄吉疾迹籍", jia: "家加假价架甲佳嘉嫁稼",
  jian: "见间件建简坚剑渐减检肩艰监箭", jiang: "将讲江降奖疆蒋匠酱", jiao: "叫教交角觉较脚骄焦胶蕉", jie: "界解接节姐借皆街结阶届戒杰洁",
  jin: "进金今近尽仅紧禁锦津筋斤", jing: "经京精景静境惊竟敬镜井颈晶", jiu: "就九久酒旧究救舅纠", ju: "局居句具举剧巨聚拒距俱菊",
  juan: "卷捐倦绢眷", jue: "觉决绝角爵掘倔", jun: "军均君俊菌郡", kai: "开慨凯楷", kan: "看刊堪砍坎",
  kang: "康抗慷扛糠", kao: "考靠烤拷", ke: "可科刻客课颗渴壳咳", ken: "肯恳啃垦", kong: "空恐孔控",
  kou: "口扣寇叩", ku: "苦库哭酷裤枯", kua: "跨夸垮挎", kuai: "快块会筷", kuan: "宽款", kuang: "况狂旷框矿筐",
  kui: "亏愧魁窥葵", kun: "困昆坤捆", kuo: "括扩阔廓", la: "拉啦辣蜡腊", lai: "来赖莱徕", lan: "兰蓝栏拦篮懒览烂",
  lang: "浪狼郎朗廊", lao: "老劳牢姥捞涝", le: "了乐勒", lei: "类泪累雷垒蕾", leng: "冷愣棱",
  li: "里理力立利历离礼丽例励梨璃黎粒", lian: "连脸练联怜莲帘廉链", liang: "两量良亮凉粮梁辆晾", liao: "了料聊辽疗寥僚",
  lie: "列烈猎裂劣", lin: "林临邻淋麟磷", ling: "另令领灵零龄岭铃凌", liu: "六流留刘柳溜硫", long: "龙隆笼聋垄拢",
  lou: "楼露漏陋搂", lu: "路鲁录陆鹿炉芦卢颅", lv: "旅律绿虑率履驴", luan: "乱卵峦孪", lue: "略掠",
  lun: "论轮伦沦仑", luo: "落罗络骆逻裸锣", ma: "吗妈马嘛麻骂码抹", mai: "买卖麦迈脉埋", man: "满慢漫曼瞒馒",
  mang: "忙茫盲芒莽", mao: "毛猫冒矛貌茅茂锚", me: "么", mei: "没美每妹梅媒煤眉枚霉", men: "们门闷",
  meng: "梦蒙猛盟孟朦", mi: "米密迷秘蜜谜弥", mian: "面免眠棉绵勉缅", miao: "秒苗庙妙描瞄", mie: "灭蔑",
  min: "民敏悯皿闽", ming: "明名命鸣冥铭", miu: "谬", mo: "莫么没末模墨默磨魔漠抹陌", mou: "某谋眸",
  mu: "木目母墓幕牧慕穆姆", na: "那拿哪纳娜呐", nai: "乃奶耐奈", nan: "南男难喃楠", nang: "囊",
  nao: "脑闹恼挠", ne: "呢呐", nei: "内馁", nen: "嫩", neng: "能", ni: "你尼泥逆拟腻妮",
  nian: "年念捻碾廿", niang: "娘酿", niao: "鸟尿袅", nie: "捏聂孽涅", nin: "您", ning: "宁凝拧柠",
  niu: "牛扭纽钮", nong: "农浓弄脓", nu: "怒奴努", nv: "女", nuan: "暖", nue: "虐疟",
  nuo: "诺挪懦糯", ou: "欧偶鸥呕藕", pa: "怕爬帕趴琵", pai: "派排拍牌徘", pan: "判盘盼畔攀潘",
  pang: "旁胖庞螃", pao: "跑炮抛泡袍咆", pei: "配陪培佩赔沛", pen: "喷盆", peng: "朋碰彭棚蓬捧膨",
  pi: "批皮披疲匹脾辟屁劈", pian: "片篇便偏骗", piao: "票飘漂朴瓢", pie: "撇瞥", pin: "品贫拼频聘",
  ping: "平评凭瓶屏萍苹", po: "破迫婆坡颇泊泼魄", pou: "剖", pu: "普铺扑仆葡蒲朴瀑", qi: "起其七期气奇齐企妻器骑棋旗弃泣启契",
  qia: "恰洽掐", qian: "前千钱浅签迁潜遣谦铅歉", qiang: "强枪墙抢腔蔷", qiao: "桥悄巧敲乔侨翘壳", qie: "且切窃怯茄",
  qin: "亲琴侵勤秦禽钦寝", qing: "情清青轻请晴庆倾卿氢", qiong: "穷琼穹", qiu: "求秋球丘囚邱蚯", qu: "去取区曲趣驱屈渠娶",
  quan: "全权劝圈泉犬券拳", que: "却确缺雀瘸", qun: "群裙", ran: "然染燃冉", rang: "让壤嚷攘",
  rao: "绕扰饶", re: "热惹", ren: "人任认忍仁刃韧", reng: "仍扔", ri: "日", rong: "容荣融绒溶榕",
  rou: "肉柔揉", ru: "如入儒乳辱汝濡", ruan: "软阮", rui: "瑞锐蕊", run: "润闰", ruo: "若弱",
  sa: "撒萨飒", sai: "赛塞腮", san: "三散伞叁", sang: "桑丧嗓", sao: "扫嫂骚臊", se: "色涩瑟",
  sen: "森", seng: "僧", sha: "杀沙傻纱刹砂莎", shai: "晒筛", shan: "山善闪衫扇珊删单",
  shang: "上商伤尚赏裳", shao: "少烧绍稍哨勺梢", she: "社设舍射涉蛇舌奢", shei: "谁", shen: "什身深神甚审伸沈慎渗",
  sheng: "生声省胜升盛圣剩牲绳", shi: "是时事十师史世石识实式施示失士市室始诗适氏释誓狮", shou: "手受首收守授瘦兽寿售", shu: "书数树属术述束熟叔署鼠蔬舒殊",
  shua: "刷耍", shuai: "摔甩帅衰", shuan: "拴栓", shuang: "双霜爽", shui: "水说谁睡税", shun: "顺瞬舜",
  shuo: "说数硕烁朔", si: "四思死司丝私似寺撕斯饲", song: "送松宋颂耸诵", sou: "搜艘嗽擞", su: "苏速俗诉宿素肃粟",
  suan: "算酸蒜", sui: "随岁虽碎遂髓", sun: "孙损笋", suo: "所索锁缩琐", ta: "他她它踏塔塌",
  tai: "太台态抬泰胎苔", tan: "谈探坦叹摊贪滩坛檀", tang: "唐堂汤糖躺倘趟膛", tao: "讨逃桃套涛陶掏淘葡萄", te: "特",
  teng: "疼腾藤誉", ti: "体提题替梯踢啼惕", tian: "天田填甜添恬", tiao: "条调跳挑迢", tie: "铁贴帖",
  ting: "听停庭挺亭艇厅", tong: "同通统童痛铜桶筒", tou: "头投透偷", tu: "土图突途徒涂兔吐屠", tuan: "团湍",
  tui: "推退腿颓褪", tun: "吞屯臀", tuo: "托脱拖妥驼陀拓", wa: "瓦挖娃蛙洼袜", wai: "外歪",
  wan: "完万晚湾玩碗弯挽顽丸", wang: "王望往忘网亡旺汪枉", wei: "为位未味委围维唯微危伟卫尾慰威胃谓", wen: "文问温闻稳纹吻蚊瘟",
  weng: "翁嗡", wo: "我握窝卧沃涡", wu: "无五物午屋武舞务误悟雾巫侮乌", xi: "西习喜系息希席细昔吸戏惜悉析溪夕牺膝",
  xia: "下夏吓狭霞瞎峡侠匣", xian: "先现见线限显险仙鲜闲贤弦嫌县献宪", xiang: "想向相象香像响项乡降详享箱祥巷", xiao: "小笑消校效晓销肖萧潇孝宵",
  xie: "写些谢解协邪斜血鞋携胁歇", xin: "心新信辛欣薪馨衅", xing: "行形星性兴幸型醒姓刑杏腥", xiong: "兄胸雄凶熊匈",
  xiu: "修秀休袖宿羞绣锈嗅", xu: "须需许续序虚徐蓄叙绪恤", xuan: "选悬宣玄旋轩喧绚", xue: "学雪血削穴靴薛",
  xun: "迅寻训讯巡旬循勋熏", ya: "呀压亚牙芽雅押鸭涯衙", yan: "言眼演严烟延沿研颜掩厌宴雁焰盐岩", yang: "样阳洋央养扬羊仰痒杨",
  yao: "要摇药遥腰咬邀耀谣妖尧", ye: "也夜业叶野爷页液耶", yi: "一以已意医易依移衣义议艺亿役异疑亦翼译遗椅", yin: "因音引银印饮阴隐吟瘾姻",
  ying: "应影英营迎硬映赢鹰婴莹樱", yong: "用永拥勇涌泳庸咏踊", you: "有又由友游右油优忧尤邮诱幽犹", yu: "与于语育余雨遇域欲玉鱼愈宇予狱羽誉愚",
  yuan: "远原元院员园愿源缘怨猿冤", yue: "月越约乐跃阅岳钥悦", yun: "云运允孕晕韵蕴酝", za: "杂砸咱扎",
  zai: "在再载灾宰栽仔", zan: "赞暂攒咱", zang: "藏脏葬赃", zao: "早造遭糟燥皂藻", ze: "则责择泽仄",
  zei: "贼", zen: "怎", zeng: "曾增赠憎", zha: "扎炸查诈闸眨渣", zhai: "斋宅摘债窄寨",
  zhan: "站展战占沾斩盏毡瞻", zhang: "长张章丈掌障帐涨杖彰", zhao: "着找照朝招赵罩兆沼", zhe: "这者着折哲浙遮辙",
  zhen: "真阵针震镇珍枕振侦贞", zheng: "正整争政证征挣郑症蒸", zhi: "之只知直制至治指支志值织纸止质智职置执植殖脂", zhong: "中种重终众钟忠肿仲",
  zhou: "周州洲宙粥舟骤轴昼", zhu: "主住注助著诸属猪竹烛逐祝珠株筑柱", zhua: "抓爪", zhuai: "拽",
  zhuan: "传专转撰砖赚篆", zhuang: "装状庄撞壮桩妆", zhui: "追坠缀椎", zhun: "准谆", zhuo: "着桌捉浊啄灼拙",
  zi: "子自字资紫姿滋仔兹咨", zong: "总宗纵踪棕综", zou: "走奏揍邹", zu: "足族组祖阻租卒",
  zuan: "钻纂", zui: "最罪嘴醉", zun: "尊遵", zuo: "作做坐左座昨佐",
};

function v2Shelf() {
  if (state.libOpds) return v2OpdsScreen();
  if (state.libBook) {
    const b = LIB_BOOKS.find((x) => x.id === state.libBook);
    return `${bar()}<main class="v-page">${v2Head(esc(b.title), LIB_SOURCES[b.lib].short)}<div class="v-pane v-lib">${libDetail(b)}</div></main>`;
  }
  const q = state.libQuery;
  const all = shelfItems(q);
  const count = q.trim() ? L(`找到 ${all.length} 本`, `${all.length} found`) : L(`${all.length} 本`, `${all.length} books`);
  const searchBtn = `<button class="v-ltag-btn" data-shelf-search="1">${L("搜书", "Search")}</button>`;
  const libBtn = `<button class="v-ltag-btn" data-opds="1">${L("书库", "Libraries")}</button>`;
  const view = `<div class="seg tiny v-lview"><button data-shelf="cover" class="${state.shelfView === "cover" ? "on" : ""}">${t("covers")}</button><button data-shelf="list" class="${state.shelfView === "list" ? "on" : ""}">${t("list")}</button></div>`;
  const search = `<label class="v-lsearch">${ic("search")}<input data-lib-q value="${esc(q)}" placeholder="${L("书名、作者或拼音首字母", "Title or author")}">${q ? `<button data-lib-clear>${L("清空", "Clear")}</button>` : ""}</label>`;
  const body = !all.length ? `<p class="v-lhint">${L("没找到。换个词，或者只打拼音首字母试试。", "Nothing. Try other words.")}</p>`
    : state.shelfView === "list"
      ? renderShelfList(all)
      : renderShelfGrid(all, "gallery");
  const rightAct = `<div class="v-lhead-acts">${searchBtn}${libBtn}${view}</div>`;
  return `${bar()}<main class="v-page" style="position:relative">${v2Head(L("书架", "Shelf"), count, rightAct)}<div class="v-pane v-lib">${search}${body}</div>${renderSearchKbOverlay()}</main>`;
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
  if (!nav) {
    // 书库配置主页：微信读书扫码登录 + 已订阅 OPDS 书库 + 添加/管理书库
    const wrRow = state.wereadLoggedIn
      ? `<button class="v-opds-row" data-weread-shelf="1"><span><b>${L("微信读书 · 我的书架", "WeRead · My Shelf")}</b><em>${L("已登录 · 同步云端书架、已购书籍与阅读进度", "Signed in · Syncs shelf, purchases & progress")}</em></span><small>›</small></button>`
      : `<button class="v-opds-row" data-weread-qr="1"><span><b>${L("微信读书 · 扫码登录", "WeRead · Sign In")}</b><em>${L("未登录 · 扫码同步微信读书书架与已购书籍", "Sign in to sync your WeRead shelf & purchases")}</em></span><small>›</small></button>`;
    const customLibs = state.libs.filter((id) => id !== "weread").map((id) => LIB_SOURCES[id]).filter(Boolean);
    const libRows = customLibs.map((s) => {
      const host = (() => { try { return new URL(s.url).host; } catch { return s.url; } })();
      const meta = `${host} · ${s.rich ? L("进度同步", "Syncs progress") : "OPDS"}${s.auth || s.rich ? L(" · 已认证", " · Authenticated") : ""}`;
      return `<button class="v-opds-row" data-opds-lib="${s.id}"><span><b>${esc(s.name)}</b><em>${esc(meta)}</em></span><small>›</small></button>`;
    }).join("");
    const addRow = `<button class="v-opds-row" data-lib-phone="1"><span><b>${L("+ 添加 / 管理书库订阅", "+ Add or Manage Libraries")}</b><em>${L("手机扫码粘贴 OPDS 公网地址与认证口令", "Scan with phone to add OPDS URL & password")}</em></span><small>›</small></button>`;
    const qrModal = state.wereadQrOpen ? `
      <div class="v-sheet-mask" style="position:absolute;inset:0;background:rgba(0,0,0,.35);display:flex;align-items:flex-end;z-index:20">
        <div style="width:100%;background:#fff;border-top:2px solid #000">
          <div style="background:#000;color:#fff;padding:8px 14px;display:flex;justify-content:space-between;align-items:center">
            <b>${L("微信读书扫码登录", "WeRead Sign In")}</b>
            <button data-weread-qr-close="1" style="color:#fff;text-decoration:underline;background:none;border:0;cursor:pointer">${L("关闭", "Close")}</button>
          </div>
          <div style="padding:18px 16px;display:flex;flex-direction:column;align-items:center;gap:10px;text-align:center">
            <div class="qr" style="width:120px;height:120px;border:2px solid #000;border-radius:8px;display:grid;place-items:center;font-weight:700">QR</div>
            <b style="font-size:12px">${L("请用手机微信扫码确认登录", "Scan with WeChat to sign in")}</b>
            <span style="font-size:10px;color:#333">${L("扫码同步微信读书书架、已购书籍与阅读进度", "Sign in to sync your shelf, purchases and progress.")}</span>
            <button class="v-line" data-weread-login-ok="1" style="margin-top:4px">${L("模拟：手机已扫码确认", "Simulate scan confirm")}</button>
          </div>
        </div>
      </div>` : "";
    return `${bar()}<main class="v-page" style="position:relative">${v2Head("")}<div class="v-pane v-lib"><div class="v-lhead-row"><b>${L("书库", "Libraries")}</b></div><div class="v-opds-list">${wrRow}${libRows}${addRow}</div></div>${qrModal}</main>`;
  }
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
  const searchBtn = `<button class="v-ltag-btn" data-opds-search="1">${L("换个词", "Search")}</button>`;
  const searchBar = `<div class="v-lhead-row"><b>${esc(title)}</b>${searchBtn}</div>`;
  const body = rows.length ? `<div class="v-opds-list">${rows.join("")}</div>`
    : `<p class="v-lhint">${L("这里没有书。换个词，或回上一层看看。", "Nothing here.")}</p>`;
  const pg = entries.length ? `<div class="v-lbar"><span>${L(`${entries.length} 项`, `${entries.length} items`)}</span></div>` : "";
  return `${bar()}<main class="v-page" style="position:relative">${v2Head("")}<div class="v-pane v-lib">${searchBar}${pg}${body}</div>${renderSearchKbOverlay()}</main>`;
}
function libSettingsPane() {
  const rows = libOn().map((s) => `<div class="v-row"><span><b>${esc(s.name)}</b><em>${esc(new URL(s.url).host)}${s.auth || s.rich ? L(" · 已认证", " · Auth") : ""}</em></span>
      <small class="v-lok">${s.rich ? L("带进度", "Syncs progress") : "OPDS"}</small><button class="v-line" data-lib-remove="${s.id}">${L("移除", "Remove")}</button></div>`).join("");
  const add = state.libAdding
    ? `<div class="v-fld"><label>${L("订阅链接", "Link")}</label><div class="v-inputs one"><input data-lib-url placeholder="https://…/opds"></div>
        <div class="v-two"><button class="v-line" data-lib-add-cancel>${L("取消", "Cancel")}</button><button class="v-solid" data-lib-add-save>${L("订阅", "Subscribe")}</button></div></div>`
    : `<div class="v-two"><button class="v-line" data-lib-phone>${L("在手机上填", "From your phone")}</button><button class="v-line" data-lib-add>${L("在这里打", "Type it here")}</button></div>
      <p class="v-tip">${L("链接和认证口令用手机或电脑粘贴更省事：同一个 Wi-Fi 下扫码打开配置页，在「书库订阅」里填写。", "Long links and passwords are easier to paste from a phone.")}</p>`;
  const fake = state.libs.includes("gutenberg") ? "" : `<button class="v-line dashed" data-lib-fake="gutenberg">${L("假的：再订一个古登堡（看多个书库）", "Fake: add Gutenberg")}</button>`;
  return `${rows ? `<nav class="v-rows">${rows}</nav>` : `<div class="v-card bad">${ic("x")}<span><b>${L("还没有订阅书库", "No library yet")}</b><em>${L("订了以后，书库里的书和本机的书放在一个书架上", "Subscribed books join the shelf")}</em></span></div>`}
    <p class="v-tip">${L("填公网 OPDS 链接与认证口令，可以订好几个。书库的书和本机的书放在一个书架上，封面角上斜贴着来自哪个书库；搜书时一起搜。", "Add OPDS links. Their books join the shelf, tagged on the corner.")}</p>
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
    log(`《${b.title}》书库里还没有：POST take_url，从${LIB_SOURCES[b.lib].name}收进书库，设备每 2 秒问一次 job，done 后拿 download_url。`);
    render();
    later(`libt-${id}`, 2000, () => { delete state.libTake[id]; b.lib = "custom"; b.via = undefined; b.status = "unread"; start(); });
  } else if (b.convert) {
    state.libConvert[id] = true;
    log(`《${b.title}》只有 MOBI：请书库转成 EPUB，转好再下。`);
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
  if ((el = hit("[data-shelf-list]"))) {
    state.shelfListStyle = el.dataset.shelfList;
    const s = SHELF_LIST_STYLES.find((x) => x.id === state.shelfListStyle);
    if (s) log(`切换列表视图排版为「${s.label}」：${s.desc}。`);
    flash();
    return render();
  }
  if ((el = hit("[data-cover-tpl]"))) {
    state.coverTpl = el.dataset.coverTpl;
    log(`切换无图封面模板为「${state.coverTpl}」。`);
    flash();
    return render();
  }
  // 书架一级「搜书」按钮与软键盘中英拼音输入
  if (hit("[data-shelf-search]") || hit("[data-opds-search]")) {
    state.kbOpen = true;
    state.kbZh = state.kbZh !== false;
    state.kbPy = "";
    state.kbCandPage = 0;
    state.kbText = state.libOpdsQuery || "";
    flash();
    return render();
  }
  if (hit("[data-kb-close]")) { state.kbOpen = false; state.kbPy = ""; flash(); return render(); }
  if (hit("[data-kb-lang]")) {
    if (state.kbPy) { state.kbText = (state.kbText || "") + state.kbPy; state.kbPy = ""; }
    state.kbZh = !(state.kbZh !== false);
    state.kbCandPage = 0;
    return render();
  }
  if ((el = hit("[data-kb-char]"))) {
    const ch = el.dataset.kbChar;
    if (state.kbZh !== false) {
      state.kbPy = ((state.kbPy || "") + ch).slice(0, 12);
      state.kbAssoc = "";
      state.kbCandPage = 0;
    } else {
      state.kbText = (state.kbText || "") + ch;
    }
    return render();
  }
  if ((el = hit("[data-kb-cand]"))) {
    const idx = Number(el.dataset.kbCand);
    const cands = simGetKbCandidates();
    const picked = cands[(state.kbCandPage || 0) * 7 + idx];
    if (picked) {
      state.kbText = (state.kbText || "") + picked;
      state.kbPy = "";
      state.kbAssoc = [...picked].slice(-1)[0] || "";
      state.kbCandPage = 0;
    }
    return render();
  }
  if ((el = hit("[data-kb-page]"))) {
    state.kbCandPage = Math.max(0, (state.kbCandPage || 0) + Number(el.dataset.kbPage));
    return render();
  }
  if (hit("[data-kb-bs]")) {
    if (state.kbZh !== false && state.kbPy) {
      state.kbPy = state.kbPy.slice(0, -1);
      state.kbCandPage = 0;
    } else {
      state.kbAssoc = "";
      state.kbText = [...(state.kbText || "")].slice(0, -1).join("");
    }
    return render();
  }
  if (hit("[data-kb-space]")) {
    if (state.kbZh !== false && state.kbPy) {
      const cands = simGetKbCandidates();
      const first = cands[0] || state.kbPy;
      state.kbText = (state.kbText || "") + first;
      state.kbPy = "";
      state.kbAssoc = [...first].slice(-1)[0] || "";
      state.kbCandPage = 0;
    } else {
      state.kbText = (state.kbText || "") + " ";
    }
    return render();
  }
  if (hit("[data-kb-ok]")) {
    if (state.kbZh !== false && state.kbPy) {
      const first = (SIM_PY_DICT[state.kbPy] || "")[0] || state.kbPy;
      state.kbText = (state.kbText || "") + first;
      state.kbPy = "";
    }
    const q = (state.kbText || "").trim();
    if (!q) return;
    state.kbOpen = false;
    state.libOpds = true;
    state.libOpdsQuery = q;
    state.libOpdsBrowse = "search";
    flash();
    return render();
  }
  // 书架 → 书库配置页
  if (hit("[data-opds]")) { state.libOpds = true; state.libOpdsBrowse = null; state.libOpdsQuery = ""; flash(); return render(); }
  // 书库配置页内部
  if (hit("[data-weread-qr]")) { state.wereadQrOpen = true; flash(); return render(); }
  if (hit("[data-weread-qr-close]")) { state.wereadQrOpen = false; flash(); return render(); }
  if (hit("[data-weread-login-ok]")) {
    state.wereadQrOpen = false;
    state.wereadLoggedIn = true;
    toast(L("微信读书登录成功", "WeRead signed in"));
    flash();
    return render();
  }
  if (hit("[data-weread-shelf]")) { state.libOpdsBrowse = "new"; flash(); return render(); }
  if ((el = hit("[data-opds-lib]"))) { state.libOpdsBrowse = "new"; flash(); return render(); }
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
    log(`${b.wish ? "加到" : "移出"}想读：书库同步更新。`);
    return render();
  }
  if ((el = hit("[data-lib-read]"))) {
    const b = LIB_BOOKS.find((x) => x.id === Number(el.dataset.libRead));
    log(`打开《${b.title}》：先问书库读到哪里${b.progress ? `（${Math.round(b.progress * 100)}%）` : ""}，比本机新就跳过去。假壳子里打开的是三国演义的正文。`);
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
    const id = v.includes("gutenberg") ? "gutenberg" : "custom";
    if (!state.libs.includes(id)) state.libs.push(id);
    toast(L(`已订阅「${LIB_SOURCES[id].short}」`, "Subscribed"));
    return;
  }
});
