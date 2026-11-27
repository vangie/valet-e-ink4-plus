// 页面切换和百问会话。照固件的导航栈：进页压栈，菜单 / 上一页出栈。
const stack = [];

function stopSession() {
  ["listen", "reply", "say", "live1", "live2", "live3"].forEach(cancel);
  state.live = false;
  state.listening = false;
  state.speaking = false;
  state.caption = "";
  // 自动模式下通话中该换网没换的，挂断后换
  if (state.linkPending && typeof settleLink === "function") settleLink(L("通话结束", "Call ended"));
}

function enterAi(from) {
  state.aiBack = from;
  if (isV2()) return v2EnterAi(from);
  // 从正文进来：fromPage 记住页码，默认是书的会话
  // 从其他地方进来：独立会话
  if (from === "reader") {
    state.fromPage = state.page;
    state.sessionType = "book";
  } else {
    state.fromPage = null;
    state.sessionType = "standalone";
  }
  state.answered = false;
  state.answerPage = 0;
  state.liveMuted = false;
  const name = PROVIDERS[state.provider];
  const live = state.provider === "agent" && state.mode === "live";
  const what = live ? "通话" : "问答";
  if (state.sessionType === "book") {
    log(`离开第 ${state.page + 1} 页进入百问（${name} · ${what}）。${state.provider === "agent" ? "这一页一起带上。" : "官方这条不带书。"}`);
  } else {
    log(`从${names()[from]}进入百问（${name} · ${what}）。独立会话。`);
  }
  if (live) startLive();
}

// deeper = 往更深一层走（新设计里从设置进配网、翻页器，回来还在原来那一项）
function leave(from, deeper = false) {
  if (from === "ai") {
    if (isV2()) v2LeaveAi();
    stopSession();
    state.floatExpanded = false;
  }
  if (from === "reader") {
    state.sheet = false;
    // 离开正文（不是展开悬窗）：悬窗收起，通话挂断
    if (state.float) { state.float = false; stopSession(); }
  }
  if (from === "settings") { state.wifiTest = false; state.diagnostic = ""; }
  if (from === "settings" && !(deeper && isV2())) {
    state.settingsScreen = "list";
    v2.direct = null;
  }
}

function go(screen) {
  if (screen === state.screen) return;
  const from = state.screen;
  leave(from, true);
  stack.push(from);
  state.screen = screen;
  if (screen === "ai") enterAi(from);
  flash();
  render();
}

// 直接进百问并以 Live 通话方式开始（从首页 Live 入口）
function goLive(from) {
  const prev = from || state.screen;
  leave(prev);
  stack.push(prev);
  state.screen = "ai";
  // 强制 live 模式，不管 state.mode 默认值
  if (prev === "reader") {
    state.fromPage = state.page;
    state.sessionType = "book";
  } else {
    state.fromPage = null;
    state.sessionType = "standalone";
  }
  state.answered = false;
  state.answerPage = 0;
  state.liveMuted = false;
  log(`进入通话（自建）。${state.sessionType === "book" ? `带上第 ${state.page + 1} 页。` : "独立通话。"}`);
  startLive();
  flash();
  render();
}

function back() {
  if (closeDiagnosticOverlay()) return;
  if (isV2() && v2Back()) return;
  if (state.screen === "home") return;
  const from = state.screen;
  leave(from);
  state.screen = stack.pop() || "home";
  if (from === "ai" && state.screen === "reader") log(`回到第 ${state.page + 1} 页。${isV2() ? "这场对话留着，" : "会话关掉，"}页码没动。`);
  flash();
  render();
}

function goHome() {
  leave(state.screen);
  stack.length = 0;
  state.screen = "home";
  if (isV2()) state.homePage = 0; // 新设计：回首页总是回到「今天」那一屏
  flash();
  render();
}

// 切换 AI 页里的会话：书的会话 ↔ 独立会话
function switchSession(type) {
  if (state.provider !== "agent") return;
  if (state.sessionType === type) return;
  stopSession();
  state.sessionType = type;
  state.answered = false;
  state.answerPage = 0;
  if (type === "book") {
    if (state.fromPage !== null) {
      log(`切到书的会话：第 ${state.fromPage + 1} 页。`);
    } else {
      log("没有带书进来，无法切到书的会话。");
      state.sessionType = "standalone";
      return render();
    }
  } else {
    log("切到独立会话。");
  }
  if (state.live) startLive();
  render();
}

// 问答：按住说话，松开发送；回答时短按打断。
function startListening() {
  if (state.speaking) {
    cancel("reply");
    state.speaking = false;
  }
  state.listening = true;
  render();
}

function stopListeningAndSend() {
  if (!state.listening) return;
  if (isV2() && !v2Commit()) {
    state.listening = false;
    return render();
  }
  state.listening = false;
  state.speaking = state.tts;
  state.answered = true;
  state.answerPage = 0;
  log(`松开发送：「${questionFor()}」${state.tts ? "" : "（已静音，只显示文字）"}`);
  render();
  if (state.tts) {
    later("reply", 1800, () => {
      state.speaking = false;
      render();
    });
  }
}

function abortSpeaking() {
  if (!state.speaking) {
    log("短按 AI 唤醒：没有在回答，不做事。");
    return;
  }
  cancel("reply");
  state.speaking = false;
  log("短按 AI 唤醒：打断回答，回到待命。");
  render();
}
