// 假模拟器的数据和状态。不连设备，不连网关。
const pages = [
  [
    "话说天下大势，分久必合，合久必分。周末七国分争，并入于秦。及秦灭之后，楚、汉分争，又并入于汉。",
    "汉朝自高祖斩白蛇而起义，一统天下，后来光武中兴，传至献帝，遂分为三国。",
  ],
  [
    "建宁二年四月望日，帝御温德殿。方升座，殿角狂风骤起。只见一条大青蛇，从梁上飞将下来，蟠于椅上。",
    "帝惊倒，左右急救入宫，百官俱奔避。须臾，蛇不见了。忽然大雷大雨，加以冰雹，落到半夜方止，坏却房屋无数。",
  ],
  [
    "建宁四年二月，洛阳地震；又海水泛溢，沿海居民，尽被大浪卷入海中。光和元年，雌鸡化雄。",
    "六月朔，黑气十余丈，飞入温德殿中。秋七月，有虹现于玉堂；五原山岸，尽皆崩裂。",
  ],
];

// 「自建」带着这一页去问时的回答，一项是一页。
const answers = [
  ["开篇「分久必合，合久必分」是全书的总纲。", "后面从周末七国说到汉分三国，一百二十回都在演这句话。"],
  ["这一页写温德殿的怪事：青蛇蟠椅，雷雨冰雹。", "作者用异象预示汉室将乱。这一回后面才写到张角和黄巾。"],
  ["这一页接着写灾异：地震、海水泛溢、雌鸡化雄。", "都在铺垫天下将乱。这一回后面就写到张角和黄巾。"],
];
// 通话里「自建」说的话，按从哪一页进来挑。
const liveLines = [
  ["你在读开篇。「分久必合，合久必分」是全书的总纲。", "你觉得这句话放到今天还成立吗？"],
  ["这一页是温德殿的青蛇。作者拿异象预示汉室要乱。", "后面黄巾起事，就是这些征兆落了地。"],
  ["这一页一连串灾异，都在铺垫天下大乱。", "这一回后面就写到张角了，要接着聊吗？"],
];
const liveNoBook = ["没带书。你想聊三国里的哪一段？", "可以先说一个人物，我们从他讲起。"];

const PROVIDERS = { baiwen: "百问", xiaozhi: "小智", yunxin: "网易", agent: "自建" };

const state = {
  design: "v2", // "v2" 新设计（随行）| "classic" 原始设计（官方 2.0.80）
  screen: "home",
  edition: "cn",
  vcomRead: false,
  vcom: 1200,
  blackDepth: "standard",
  bluetoothVersion: "1.0 (demo)",
  diagnostic: "",
  wifiTest: false,
  page: 0,
  sheet: false,
  float: false,         // 新设计：正文页下方的问答悬窗开着
  floatExpanded: false, // 从悬窗展开到完整助手页，返回时回到悬窗
  sheetTab: 3,
  homePage: 0,
  // 设置页：list = 分类列表，其他值 = 具体分类 ID
  settingsScreen: "list",
  settingsTab: "net",
  settingsPage: 0,
  todos: [
    { text: "买宣纸", done: false },
    { text: "把扩展字库拷到 SD", done: false },
    { text: "续读三国演义第一回", done: true },
  ],
  provider: "agent",
  pendingProvider: "agent",
  mode: "ask", // 「自建」默认进入：ask 问答 | live 通话
  aiBack: "home",
  fromPage: null,     // 从正文进来时是第几页，null = 独立会话
  sessionType: "standalone", // "book" | "standalone"，当前展示的是哪一场
  live: false,
  listening: false,
  speaking: false,
  liveMuted: false,
  answered: false,
  answerPage: 0,
  q: "",  // 新设计：这一场问的那句和回答，分页
  a: [],
  caption: "",
  liveIndex: 0,
  toast: "",
  link: "4G",
  sim: "int",
  battery: 70,
  charging: false,
  volume: 60,
  light: "both",
  brightness: 40,
  theme: "slash",
  haptic: "on",
  hapticLevel: "mid",
  chargeMa: 1000,
  lang: "zh",
  tts: true,
  idleMhz: 320,
  netGrace: 30,
  proxyWifi: false,  // 连 Wi-Fi 时走代理
  proxyCell: false,  // 用 4G 时走代理
  proxyKind: "http", // http | socks5 | ss（Shadowsocks）| trojan
  proxySsPass: false, // SS / Trojan 的密码填了没有（两种共用这一个，和设备一样只存一个密码）
  proxySni: "",      // Trojan 的服务器名（SNI），空 = 用地址
  proxyInsecure: false, // Trojan 不检查服务器证书（节点写着 skip-cert-verify=true 时）
  proxyMethod: "aes-256-gcm", // SS 的加密方式（设备支持的 12 种之一），在弹出列表里选
  proxyPicker: false, // 开着的弹出列表：false | "proto"（更多协议里的协议）| "cipher"（SS 的加密方式）
  proxyMore: "ss",   // 「更多协议」上次用的协议：ss | trojan
  proxyAddr: "",     // 地址:端口，空 = 没设置
  proxyUser: "",     // 账号，空 = 不用
  proxyTest: "",     // 上一次测试的结果
  refresh: "fast",
  shakeTurn: false,
  shakeLevel: 3,
  logOn: false,
  pagerAuto: false,  // 翻页器：开机和退出待机后自动找附近的翻页器
  usb: false,
  taskTab: "todo",
  taskOpen: -1,
  cloudTab: "app",
  cloudFilter: "all",
  localService: false,
  localServiceMode: "ap",
  localServiceUrl: "http://192.168.4.1",
  localServiceSsid: "Valet-EInk-4821",
  localServicePassword: "48214821",
  setupBackend: "openclaw",
  setupPromptCopied: false,
  setupPair: "",
  localPage: "home",
  assistantSetup: false,
  setupPhase: "waiting",   // waiting 等人打开配置页 / opened 页已打开 / connecting 在连接测试 / done / failed
  setupNote: "",           // connecting 时设备屏幕上写的那一步
  setupResult: null,       // { id, name, backend, caps }
  assistantAdvanced: false,
  assistantUnpair: false,
  shelfView: "cover",
  bookIndex: 0,
  wallUse: "",
  wallIndex: 0,
  appName: "",
  buddyBound: false,
  buddyMenu: false,
  pagerOn: false,
  photo: false,
  photoOpen: false,
  fontName: "MiSans",
  fontSize: 30,
  margin: "standard",
  spacing: "standard",
  para: "standard",
  turnStyle: "slide",
  writing: "horizontal",
  bookmarks: [],
  btMode: "1",
  btName: "Metalio-EINK",
  btDevices: false,
  standbyStyle: "classic",
  fontpack: false,   // SD 上有没有 extended-fonts.fontpack
  paired: true,      // 「自建」配对好了没有
  assistants: [
    { id: "home", name: "家里的助理", backend: "OpenClaw", url: "wss://gateway.home.example.com/valet/v1", code: "device-token", caps: { ask: true, live: true, ctx: true } },
    { id: "work", name: "工作助理", backend: "Hermes", url: "wss://assistant.work.example.com/valet/v1", code: "device-token", caps: { ask: true, live: true, ctx: true } },
  ],
  activeAssistant: "home",
  assistantDetail: null,
  pairStatus: "",
  gatewayKind: "OpenClaw",
  gateway: "wss://gateway.home.example.com/valet/v1",
  pair: "4821",
  standby: false,
  off: false,
  booting: false,
  bootTo: "home",
};

const panel = document.querySelector("#panel");
const logEl = document.querySelector("#log");

function log(text) {
  const li = document.createElement("li");
  li.textContent = text;
  logEl.prepend(li);
  while (logEl.children.length > 10) logEl.lastChild.remove();
}

function flash() {
  panel.classList.remove("flash");
  void panel.offsetWidth;
  panel.classList.add("flash");
}

const timers = {};
function later(name, ms, fn) {
  clearTimeout(timers[name]);
  timers[name] = setTimeout(fn, ms);
}
function cancel(name) {
  clearTimeout(timers[name]);
}

function toast(text) {
  state.toast = text;
  later("toast", 1500, () => {
    state.toast = "";
    render();
  });
  render();
}

function questionFor() {
  if (isV2()) return nowTalk().q;
  if (state.sessionType === "book" && state.fromPage !== null) return "这一页在讲什么？";
  return "三国演义从哪里开始讲？";
}

function answerFor() {
  if (isV2()) return nowTalk().a.length ? nowTalk().a : [""];
  const fromBook = state.sessionType === "book" && state.fromPage !== null;
  if (state.provider === "agent") {
    return fromBook ? answers[state.fromPage] : ["没带书，按全书来答：开篇从「分久必合，合久必分」讲起。"];
  }
  return fromBook
    ? ["我不知道你在读哪一页。把这段念给我听，我再帮你看。"]
    : ["开篇从「分久必合，合久必分」讲起。"];
}

// 假数据：把「蟠」当成主字库里没有的字。没装扩展字库时画成方框。
const RARE = /蟠/g;
function glyphs(text) {
  return state.fontpack ? text : text.replace(RARE, '<i class="tofu"></i>');
}
function missingGlyphs(page) {
  return state.fontpack ? 0 : (pages[page].join("").match(RARE) || []).length;
}
