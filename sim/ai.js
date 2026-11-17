// 百问页和设置页。
function stateWord() {
  if (state.provider === "agent" && state.live) return `通话 · ${state.muted ? "静音" : state.speaking ? "说" : "听"}`;
  if (state.listening) return "聆听中...";
  if (state.speaking) return "回答中...";
  return "待命";
}

function chip() {
  if (state.fromPage === null) return "";
  if (state.provider === "agent") {
    return `<div class="chip">带上第 ${state.fromPage + 1} 页：${esc(pages[state.fromPage][0].slice(0, 14))}…</div>`;
  }
  return `<div class="chip dim">官方百问拿不到正在读的这一页</div>`;
}

function askBody() {
  if (state.listening) return `<div class="big">聆听中...</div><p class="note">松开发送</p>`;
  if (state.answered) {
    const list = answerFor();
    return `<div class="qa"><p><b>用户：</b>${esc(questionFor())}</p><p><b>AI：</b>${esc(list[state.answerPage])}</p></div>`;
  }
  return `<div class="idle"><p>按住 AI 唤醒说话，松开发送。</p><p class="note">回答时短按 AI 唤醒是打断。</p></div>`;
}

function liveBody() {
  const fallback = state.muted ? "麦克风关着。点「取消静音」再说。" : "正在听，直接说。";
  const on = state.speaking || (state.listening && !state.muted);
  return `<div class="live"><div class="wave${on ? " on" : ""}">${"<i></i>".repeat(9)}</div>
    <p class="caption">${esc(state.caption || fallback)}</p>
    <p class="note">字幕整页换，不滚动。AI 在说时短按 AI 唤醒打断。</p></div>`;
}

function pageCount() {
  return state.answered && !state.live ? `${state.answerPage + 1} / ${answerFor().length}` : "";
}

function aiFoot() {
  if (state.provider !== "agent") {
    return `<div class="foot"><span>${PROVIDERS[state.provider]} · 按住说话</span><span>${pageCount()}</span></div>`;
  }
  const right = state.live
    ? `<button data-mute>${state.muted ? "取消静音" : "静音"}</button>`
    : `<span>${pageCount()}</span>`;
  return `<div class="foot"><span class="seg">
      <button data-mode="ask" class="${state.live ? "" : "on"}">问答</button><button data-mode="live" class="${state.live ? "on" : ""}">通话</button>
    </span>${right}</div>`;
}

function ai() {
  const body = state.provider === "agent" && state.live ? liveBody() : askBody();
  return `${bar(stateWord())}<div class="ai">${chip()}<div class="ai-main">${body}</div>${aiFoot()}</div>`;
}

function settings() {
  const choices = Object.entries(PROVIDERS).map(([id, name]) =>
    `<button class="choice${state.pendingProvider === id ? " on" : ""}" data-provider="${id}">${name}</button>`).join("");
  const mine = state.pendingProvider === "agent" ? `
    <label>网关地址<input id="gateway" value="${esc(state.gateway)}" /></label>
    <label>配对码<input id="pair" value="${esc(state.pair)}" /></label>
    <label>进百问先进入<select id="mode">
      <option value="ask"${state.mode === "ask" ? " selected" : ""}>问答</option>
      <option value="live"${state.mode === "live" ? " selected" : ""}>通话</option></select></label>
    <p class="note">改问答或通话不用重启。</p>` : `<p class="note">官方三项都是按住说话，没有通话。</p>`;
  const reboot = state.pendingProvider !== state.provider
    ? `<button class="wide" data-reboot>保存并重启</button><p class="note">换云服务要重启，官方固件也是这样。</p>` : "";
  return `${bar()}<div class="settings">
    <h3>对话 · 云服务</h3><div class="choices">${choices}</div>${mine}
    <h3>网络</h3><div class="choices">
      <button class="choice${state.link === "4G" ? " on" : ""}" data-link="4G">4G</button>
      <button class="choice${state.link === "wifi" ? " on" : ""}" data-link="wifi">Wi-Fi</button>
      <button class="choice${state.usb ? " on" : ""}" data-usb>USB ${state.usb ? "已连" : "未连"}</button>
    </div>${reboot}</div>`;
}
