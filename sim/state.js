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

// 「我的 AI」带着这一页去问时的回答，一项是一页。
const answers = [
  ["开篇「分久必合，合久必分」是全书的总纲。", "后面从周末七国说到汉分三国，一百二十回都在演这句话。"],
  ["这一页写温德殿的怪事：青蛇蟠椅，雷雨冰雹。", "作者用异象预示汉室将乱。这一回后面才写到张角和黄巾。"],
  ["这一页接着写灾异：地震、海水泛溢、雌鸡化雄。", "都在铺垫天下将乱。这一回后面就写到张角和黄巾。"],
];
// 通话里「我的 AI」说的话，按从哪一页进来挑。
const liveLines = [
  ["你在读开篇。「分久必合，合久必分」是全书的总纲。", "你觉得这句话放到今天还成立吗？"],
  ["这一页是温德殿的青蛇。作者拿异象预示汉室要乱。", "后面黄巾起事，就是这些征兆落了地。"],
  ["这一页一连串灾异，都在铺垫天下大乱。", "这一回后面就写到张角了，要接着聊吗？"],
];
const liveNoBook = ["没带书。你想聊三国里的哪一段？", "可以先说一个人物，我们从他讲起。"];

const PROVIDERS = { baiwen: "百问", xiaozhi: "小智", yunxin: "网易", agent: "我的 AI" };

const state = {
  screen: "home", // home | detail | reader | ai | settings | wallpaper | tasks | transfer
  page: 0,
  sheet: false, // 正文里的卡片：短按 AI 唤醒或点屏幕中间
  provider: "agent",
  pendingProvider: "agent",
  mode: "ask", // 「我的 AI」进百问先进入：ask 问答 | live 通话
  aiBack: "home",
  fromPage: null, // 从正文进百问时是第几页
  live: false,
  listening: false,
  speaking: false,
  muted: false,
  answered: false,
  answerPage: 0,
  caption: "",
  liveIndex: 0,
  volume: 60,
  toast: "",
  link: "4G",
  usb: false,
  gateway: "wss://gateway.local/passport/v1",
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
  return state.fromPage !== null ? "这一页在讲什么？" : "三国演义从哪里开始讲？";
}

function answerFor() {
  const fromBook = state.fromPage !== null;
  if (state.provider === "agent") {
    return fromBook ? answers[state.fromPage] : ["没带书，按全书来答：开篇从「分久必合，合久必分」讲起。"];
  }
  return fromBook
    ? ["我不知道你在读哪一页。把这段念给我听，我再帮你看。"]
    : ["开篇从「分久必合，合久必分」讲起。"];
}
