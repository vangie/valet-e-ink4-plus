// 「我的 AI」的通话：连上后一直听，AI 说完回到听。同一条连接，切问答不断线。
function lines() {
  return state.fromPage !== null ? liveLines[state.fromPage] : liveNoBook;
}

function startLive() {
  state.live = true;
  state.listening = true;
  state.speaking = false;
  state.liveIndex = 0;
  state.caption = "";
  liveTurn(1200);
}

// 假装用户说了一句，AI 接着说一段。
function liveTurn(wait) {
  later("live1", wait, () => {
    if (!state.live || state.muted) return;
    state.listening = false;
    state.speaking = true;
    state.caption = lines()[state.liveIndex % lines().length];
    state.liveIndex += 1;
    render();
    later("live2", 2600, () => {
      if (!state.live) return;
      state.speaking = false;
      state.listening = !state.muted;
      state.caption = "";
      render();
      liveTurn(3200);
    });
  });
}

function interruptLive() {
  if (!state.speaking) {
    log("短按 AI 唤醒：AI 没在说，不做事。");
    return;
  }
  cancel("live2");
  state.speaking = false;
  state.listening = true;
  state.caption = "";
  log("短按 AI 唤醒：打断。只停这段播放，连接还在。");
  render();
  liveTurn(3200);
}

function toggleMute() {
  state.muted = !state.muted;
  if (state.muted) {
    cancel("live1");
    if (!state.speaking) state.listening = false;
    log("通话静音：麦克风关，AI 正在说的话照常放完。");
  } else {
    state.listening = !state.speaking;
    log("取消静音，继续听。");
    if (!state.speaking) liveTurn(1500);
  }
  render();
}

function switchMode(mode) {
  if (state.provider !== "agent") return;
  const toLive = mode === "live";
  if (toLive === state.live) return;
  stopSession();
  state.muted = false;
  if (toLive) {
    startLive();
    log("切到通话。同一条连接，不重连网关。");
  } else {
    state.answered = false;
    state.answerPage = 0;
    log("切回问答。连接还在，按住 AI 唤醒说话。");
  }
  render();
}
