// 页面切换和百问会话。照固件的导航栈：进页压栈，菜单 / 上一页出栈。
const stack = [];

function stopSession() {
  ["listen", "reply", "say", "live1", "live2", "live3"].forEach(cancel);
  state.live = false;
  state.listening = false;
  state.speaking = false;
  state.caption = "";
}

function enterAi(from) {
  state.fromPage = from === "reader" ? state.page : null;
  state.answered = false;
  state.answerPage = 0;
  state.muted = false;
  const name = PROVIDERS[state.provider];
  const live = state.provider === "agent" && state.mode === "live";
  const what = live ? "通话" : "问答";
  if (state.fromPage !== null) {
    log(`离开第 ${state.page + 1} 页进入百问（${name} · ${what}）。${state.provider === "agent" ? "这一页一起带上。" : "官方这条不带书。"}`);
  } else {
    log(`从${NAMES[from]}进入百问（${name} · ${what}）。`);
  }
  if (live) startLive();
}

function leave(from) {
  if (from === "ai") stopSession();
  if (from === "reader") state.sheet = false;
}

function go(screen) {
  if (screen === state.screen) return;
  const from = state.screen;
  leave(from);
  stack.push(from);
  state.screen = screen;
  if (screen === "ai") enterAi(from);
  flash();
  render();
}

function back() {
  if (state.screen === "home") return;
  const from = state.screen;
  leave(from);
  state.screen = stack.pop() || "home";
  if (from === "ai" && state.screen === "reader") log(`回到第 ${state.page + 1} 页。会话关掉，页码没动。`);
  flash();
  render();
}

function goHome() {
  leave(state.screen);
  stack.length = 0;
  state.screen = "home";
  flash();
  render();
}

// 问答：按住说话，松开发送；回答时短按打断。官方和「我的 AI」一样。
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
  state.listening = false;
  state.speaking = true;
  state.answered = true;
  state.answerPage = 0;
  log(`松开发送：「${questionFor()}」`);
  render();
  later("reply", 1800, () => {
    state.speaking = false;
    render();
  });
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
