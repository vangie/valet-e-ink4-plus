// 配置页的画法和交互。数据和解析在 setup.js，提示词在 prompts.js。
(() => {
  const { S, T, lang, BACKENDS, wait, phase, deviceUrl, parse, demoReply } = SetupPage;
  const h = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  const CHECK = '<svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>';
  let el = null;
  let token = 0;
  let noteTimer;
  const promptText = () => VALET_PROMPTS.build(S.backend, lang());
  const defaultName = () => (S.backend === "hermes" ? "Hermes 助理" : "OpenClaw 助理");
  const hostOf = (url) => { try { return new URL(url).host; } catch { return url; } };
  const capsOf = (a) => ({ ask: true, live: true, ctx: true, ...(a.caps || {}) });

  function mount() {
    if (el) return;
    el = document.createElement("aside");
    el.className = "sw";
    el.hidden = true;
    el.setAttribute("role", "dialog");
    el.innerHTML = `<div class="sw-chrome"><span class="sw-lights"><i></i><i></i><i></i></span><span class="sw-url"><b></b></span><span class="sw-view"></span><button class="sw-x" data-sw-close aria-label="close">×</button></div><div class="sw-scroll"><div class="sw-page" id="sw-page"></div></div>`;
    document.body.appendChild(el);
    el.addEventListener("click", onClick);
    el.addEventListener("input", onInput);
    el.addEventListener("change", onChange);
  }

  // ── 画
  function listHtml() {
    const items = state.assistants.map((a) => {
      const cur = a.id === state.activeAssistant;
      const c = capsOf(a);
      const caps = `<span>${T("提问", "Ask")} ✓</span><span class="${c.live ? "" : "is-warn"}">${T("通话", "Call")} ${c.live ? "✓" : T("未配置", "not set up")}</span><span>${T("阅读上下文", "Reading context")} ✓</span>`;
      return `<article class="sw-item ${cur ? "is-cur" : ""}"><div><h3>${h(a.name)}${cur ? `<em class="sw-tag">${T("当前", "In use")}</em>` : ""}</h3><p>${h(a.backend)} · ${h(hostOf(a.url))}</p><p class="sw-caps">${caps}</p></div><div class="sw-act">${cur ? "" : `<button data-sw-use="${h(a.id)}">${T("使用", "Use")}</button>`}<button data-sw-rename="${h(a.id)}">${T("改名", "Rename")}</button><button class="sw-quiet" data-sw-remove="${h(a.id)}">${S.removing === a.id ? T("确认移除", "Confirm") : T("移除", "Remove")}</button></div></article>`;
    }).join("");
    return `<div class="sw-card-head"><h2>${T("我的助理", "My assistants")}</h2><span>${state.assistants.length}</span></div>${items || `<p class="sw-empty">${T("设备上还没有助理。从下面添加第一个。", "No assistant on the device yet. Add the first one below.")}</p>`}`;
  }
  function scopeHtml() {
    const rows = [
      [T("会做", "Will do"), T("检查并复用已有的插件和公网入口，缺什么补什么，生成一次性配对码", "Check and reuse the plugin and public entry you already have, add what is missing, make a one-time pairing code")],
      [T("先问你", "Asks first"), T("重启网关、改动已有的公网路由、产生新费用", "Restarting the gateway, changing an existing public route, anything that costs money")],
      [T("不会做", "Never"), T("升级 OpenClaw、索要模型密钥、把配对码发给第三方", "Upgrading the host, asking for model keys, sending the code to a third party")],
    ];
    return `<dl class="sw-scope">${rows.map(([k, v]) => `<div><dt>${k}</dt><dd>${v}</dd></div>`).join("")}</dl>`;
  }
  function stepHead(n, ok, title, sub, id) {
    return `<div class="sw-sh"><span class="sw-n ${ok ? "is-ok" : ""}" id="${id}">${ok ? CHECK : n}</span><div><h3>${title}</h3><p>${sub}</p></div></div>`;
  }
  function addHtml() {
    const name = BACKENDS[S.backend];
    const seg = Object.entries(BACKENDS).map(([k, v]) => `<label><input type="radio" name="sw-backend" value="${k}" ${S.backend === k ? "checked" : ""}><span>${v}</span></label>`).join("");
    const chips = (lang() === "en" ? ["Home assistant", "Work assistant", "Study assistant"] : ["家里的助理", "工作助理", "书房助理"]).map((c) => `<button type="button" data-sw-chip="${h(c)}">${h(c)}</button>`).join("");
    const demo = [["ok", T("全部通过", "All pass")], ["noLive", T("通话未配置", "Call not set up")], ["expired", T("配对码已过期", "Code expired")], ["unreach", T("连不上网关", "Gateway unreachable")]]
      .map(([k, v]) => `<option value="${k}" ${S.demo === k ? "selected" : ""}>${v}</option>`).join("");
    return `<div class="sw-card-head"><h2>${T("添加助理", "Add an assistant")}</h2></div><div class="sw-steps">
      <section>${stepHead(1, S.copied === "ok", T("让助理做好准备", "Get your assistant ready"), T("把提示发给你平时聊天的助理。它会在自己的主机上检查、安装，准备好公网入口，再把结果回给你。", "Send the prompt to the assistant you normally chat with. It works on its own host, sets things up and replies with the result."), "sw-n1")}
        <div class="sw-body"><fieldset class="sw-seg"><legend>${T("你的助理是", "Your assistant is")}</legend><div>${seg}</div></fieldset>
          <div class="sw-row"><button class="sw-primary" data-sw-copy id="sw-copy"></button><span class="sw-note" id="sw-copy-note" role="status"></span></div>
          <details class="sw-prompt" id="sw-prompt"><summary>${T("查看提示全文", "Read the whole prompt")} · ${promptText().length} ${T("字", "chars")}</summary><pre>${h(promptText())}</pre></details>${scopeHtml()}</div></section>
      <section>${stepHead(2, false, T("粘贴助理的回复", "Paste its reply"), T("整段粘贴就行，地址、配对码和名称会自动认出来。", "Paste the whole reply. The address, code and name are picked out for you."), "sw-n2")}
        <div class="sw-body"><textarea id="sw-raw" rows="5" spellcheck="false" autocomplete="off" placeholder='{"url":"wss://…","code":"…"}'></textarea><div id="sw-parsed" aria-live="polite"></div>
          <label class="sw-field">${T("给它起个名字", "Name it")}<input id="sw-name" maxlength="32" autocomplete="off" placeholder="${h(defaultName())}"></label><div class="sw-chips">${chips}</div></div></section>
      <section>${stepHead(3, false, T("保存到设备并连接", "Save to the device and connect"), T(`设备会连上 ${name} 的网关，测试提问、通话和阅读上下文。`, `The device connects to your ${name} gateway and tests Ask, Call and reading context.`), "sw-n3")}
        <div class="sw-body"><button class="sw-primary" data-sw-connect id="sw-connect" disabled>${T("保存到设备并连接", "Save to device and connect")}</button><ol class="sw-run" id="sw-run"></ol><div id="sw-result"></div></div></section></div>
      <div class="sw-demo"><b>${T("仅模拟器 · 真实页面没有这一块", "Simulator only · not on the real page")}</b><div class="sw-row"><button data-sw-fill="live">${T("填入演示回复", "Fill a demo reply")}</button><button data-sw-fill="nolive">${T("填入演示回复（通话未配置）", "Fill a demo reply (Call not set up)")}</button></div><label>${T("下一次测试的结果", "Next test result")}<select id="sw-demo">${demo}</select></label></div>`;
  }
  function pageHtml() {
    const net = state.localServiceMode === "ap" ? `${T("临时热点", "Hotspot")} ${state.localServiceSsid}` : "Wi-Fi Metalio-Home";
    return `<header class="sw-head"><div class="sw-brand"><span class="sw-mark">V</span><b>Valet</b><i></i><h1>${T("连接助理", "Connect an assistant")}</h1></div>
      <div class="sw-lang" data-selected="${lang()}"><span></span><button data-sw-lang="zh">中</button><button data-sw-lang="en">EN</button></div></header>
      <p class="sw-device"><i></i>Metalio E-Ink4 Plus · ${T("已连接", "connected")} · ${net} · ${prototypeVersionLabel()} · ${T("电量", "battery")} ${state.battery}%</p>
      <section class="sw-card" id="sw-list">${listHtml()}</section><section class="sw-card" id="sw-add">${addHtml()}</section>
      <p class="sw-foot">${T("这个页面由设备在局域网内提供，不经过互联网。配对码只写进设备，换成长期凭据后就作废。", "This page is served by the device on your local network, not over the internet. The pairing code is written to the device only and stops working once it is exchanged for a long-term credential.")}</p>`;
  }
  function paintAll() {
    if (!el) return;
    el.setAttribute("aria-label", T("连接助理", "Connect an assistant"));
    el.querySelector(".sw-url b").textContent = deviceUrl();
    el.querySelector(".sw-view").innerHTML = [["desktop", T("电脑", "Computer")], ["phone", T("手机", "Phone")]]
      .map(([v, name]) => `<button data-sw-view="${v}" class="${S.view === v ? "is-on" : ""}">${name}</button>`).join("");
    el.classList.toggle("phone", S.view === "phone");
    el.querySelector("#sw-page").innerHTML = pageHtml();
    el.querySelector("#sw-raw").value = S.raw;
    el.querySelector("#sw-name").value = S.name;
    paintCopy();
    paintParsed();
    paintRun();
  }
  function paintList() {
    const list = el?.querySelector("#sw-list");
    if (list) list.innerHTML = listHtml();
  }
  function setN(id, ok, n) {
    const node = el?.querySelector(`#${id}`);
    if (!node) return;
    node.classList.toggle("is-ok", ok);
    node.innerHTML = ok ? CHECK : String(n);
  }
  function paintCopy() {
    const button = el.querySelector("#sw-copy");
    const note = el.querySelector("#sw-copy-note");
    if (!button) return;
    button.textContent = S.copyFlash ? `${T("已复制", "Copied")} ✓` : T("复制提示", "Copy prompt");
    note.className = `sw-note${S.copied === "fail" ? " is-err" : ""}`;
    note.textContent = S.copied === "ok" ? T(`粘贴给能操作 ${BACKENDS[S.backend]} 网关主机的助理，等它回复。`, `Paste it to an assistant that can operate your ${BACKENDS[S.backend]} gateway host, then wait for its reply.`)
      : S.copied === "fail" ? T("没能自动复制。展开下面的提示全文，手动选中复制。", "Could not copy automatically. Open the full prompt below and copy it by hand.")
        : T("提示里没有任何密钥，可以放心发。", "The prompt holds no secrets, so it is safe to send.");
    setN("sw-n1", S.copied === "ok", 1);
  }
  function paintParsed() {
    const box = el.querySelector("#sw-parsed");
    if (!box) return;
    const p = parse(S.raw);
    if (p.name && !S.nameTouched && !S.name) {
      S.name = p.name;
      el.querySelector("#sw-name").value = S.name;
    }
    if (p.empty) box.innerHTML = `<p class="sw-hint">${T("助理的回复里有地址和配对码就行，不用整理格式。", "The reply only needs an address and a pairing code. No tidying needed.")}</p>`;
    else {
      const row = (ok, key, value) => `<div class="${ok ? "is-ok" : "is-bad"}"><dt>${key}</dt><dd>${value}</dd></div>`;
      box.innerHTML = `<dl class="sw-got">${row(p.urlOk, T("地址", "Address"), p.url ? `<code>${h(p.url)}</code>` : "—")}${row(p.codeOk, T("配对码", "Pairing code"), p.code ? `<code>${"•".repeat(10)}</code> <small>${T("已隐藏", "hidden")}</small>` : "—")}</dl>${p.problems.length ? `<ul class="sw-prob">${p.problems.map((x) => `<li>${h(x)}</li>`).join("")}</ul>` : ""}${p.ok && p.same ? `<p class="sw-warn">${T(`这个地址已经添加过（${h(p.same.name)}）。继续会用新的配对替换它。`, `This address is already saved (${h(p.same.name)}). Continuing replaces its pairing.`)}</p>` : ""}`;
    }
    setN("sw-n2", p.ok, 2);
    const button = el.querySelector("#sw-connect");
    if (button) button.disabled = !p.ok || Boolean(S.run?.busy);
  }
  function paintRun() {
    const run = S.run;
    const list = el.querySelector("#sw-run");
    const result = el.querySelector("#sw-result");
    const button = el.querySelector("#sw-connect");
    if (!list) return;
    const icon = { todo: "", run: '<i class="sw-spin"></i>', ok: CHECK, warn: "!", err: "×" };
    list.innerHTML = run ? run.steps.map((x) => `<li class="is-${x.st}"><span class="sw-dot">${icon[x.st]}</span><div><b>${x.label}</b><p>${h(x.note || x.sub)}</p></div></li>`).join("") : "";
    let banner = "";
    if (S.done) {
      const c = S.done.caps;
      banner = `<div class="sw-banner ${c.live ? "is-ok" : "is-warn"}"><b>${T("已连接", "Connected")} · ${h(S.done.name)}</b><p>${T("提问", "Ask")} ✓ · ${T("通话", "Call")} ${c.live ? "✓" : T("未配置", "not set up")} · ${T("阅读上下文", "Reading context")} ✓</p>${c.live ? "" : `<p>${T("通话要有实时语音服务。让助理补上凭据后，点「重新测试通话」。", "Calls need a realtime voice service. Once the assistant has added the credential, choose Test Call again.")}</p>`}<div class="sw-row">${c.live ? "" : `<button data-sw-retest>${T("重新测试通话", "Test Call again")}</button>`}<button data-sw-again>${T("再添加一个", "Add another")}</button><button class="sw-primary" data-sw-close>${T("回到设备", "Back to the device")}</button></div></div>`;
    } else if (run && !run.busy && run.error) {
      const at = run.steps.find((x) => x.st === "err")?.label || "";
      banner = `<div class="sw-banner is-err"><b>${T("没连上", "Could not connect")}</b><p>${T(`停在「${at}」。按上面的提示处理，再点「重试」。`, `Stopped at “${at}”. Follow the hint above, then press Try again.`)}</p></div>`;
    }
    result.innerHTML = banner;
    setN("sw-n3", Boolean(S.done), 3);
    if (button) {
      button.textContent = run?.busy ? T("正在连接…", "Connecting…") : run?.error ? T("重试", "Try again") : T("保存到设备并连接", "Save to device and connect");
      button.hidden = Boolean(S.done);
      button.disabled = !parse(S.raw).ok || Boolean(run?.busy);
    }
  }

  async function copyPrompt() {
    const text = promptText();
    let ok = false;
    try { await navigator.clipboard.writeText(text); ok = true; } catch {
      // 设备提供的页面是 http://，浏览器不给 clipboard API，退回选中再复制。
      const box = document.createElement("textarea");
      box.value = text;
      box.style.cssText = "position:fixed;top:0;opacity:0";
      document.body.appendChild(box);
      box.select();
      try { ok = document.execCommand("copy"); } catch { ok = false; }
      box.remove();
    }
    S.copied = ok ? "ok" : "fail";
    S.copyFlash = ok;
    paintCopy();
    clearTimeout(noteTimer);
    noteTimer = setTimeout(() => { S.copyFlash = false; if (el) paintCopy(); }, 4000);
  }

  // ── 保存并连接测试。设备屏幕同步显示每一步；配置页关掉后设备照样测完。
  function stepList() {
    const host = hostOf(parse(S.raw).url);
    return [
      ["save", T("写入设备", "Save to the device"), T("地址和配对码写入设备；配对码只能用一次", "Address and code are saved; the code works once"), T("已写入", "Saved")],
      ["reach", T("连接网关", "Reach the gateway"), "DNS · TLS · WebSocket", host],
      ["auth", T("设备签名握手", "Device handshake"), T("换成长期凭据，之后不再用配对码", "Swap the code for a long-term credential"), T("已换成长期凭据，配对码作废", "Long-term credential issued; the code is spent")],
      ["ask", T("提问测试", "Ask test"), T("发一句话，等回复", "Send one line, wait for the reply"), T("收到回复", "Got a reply")],
      ["live", T("通话测试", "Call test"), T("建立实时语音，等第一段声音", "Open a realtime voice, wait for first audio"), T("收到第一段声音", "Got first audio")],
      ["ctx", T("阅读上下文", "Reading context"), T("带书名和页码问一句", "Ask one line with a title and page"), T("助理认得出这一页", "The assistant recognises the page")],
    ];
  }
  async function connect() {
    const p = parse(S.raw);
    if (!p.ok || S.run?.busy) return;
    const my = ++token;
    const alive = () => my === token;
    const outcome = S.demo;
    const defs = stepList();
    S.done = null;
    S.run = { busy: true, error: "", steps: defs.map(([k, label, sub]) => ({ k, label, sub, st: "todo", note: "" })) };
    paintRun();
    const mark = (i, st, note = "") => { S.run.steps[i].st = st; S.run.steps[i].note = note; paintRun(); };
    const fail = (i, message) => {
      mark(i, "err", message);
      S.run.busy = false;
      S.run.error = message;
      paintRun();
      phase("failed", message);
    };
    const caps = { ask: true, live: true, ctx: true };
    for (let i = 0; i < defs.length; i++) {
      mark(i, "run");
      phase("connecting", defs[i][1]);
      await wait(i === 0 ? 450 : 750);
      if (!alive()) return;
      if (i === 1 && outcome === "unreach") return fail(i, T(`连不上 ${p.host}。请助理检查公网入口：DNS、证书、WebSocket 转发。设备在 4G 下也要访问得到。`, `Cannot reach ${p.host}. Ask the assistant to check the public entry: DNS, certificate, WebSocket forwarding. It must also work from 4G.`));
      if (i === 2 && outcome === "expired") return fail(i, T("配对码已过期，或已经用过（它只能用一次）。让助理重新生成一个，粘贴后重试。", "The code expired or was already used (it works once). Ask the assistant for a new one, paste it and try again."));
      if (i === 4 && outcome === "noLive") {
        caps.live = false;
        mark(i, "warn", T("未配置：网关还没有实时语音服务。提问和阅读上下文可以用。", "Not set up: the gateway has no realtime voice service. Ask and reading context still work."));
        continue;
      }
      mark(i, "ok", defs[i][3]);
    }
    const name = (S.name.trim() || p.name || defaultName()).slice(0, 32);
    const found = p.same;
    const id = found ? found.id : `assistant-${Date.now()}`;
    const entry = { id, name, backend: BACKENDS[S.backend], url: p.url, code: "device-token", caps };
    if (found) state.assistants[state.assistants.indexOf(found)] = entry;
    else state.assistants.push(entry);
    useAssistant(id);
    state.setupResult = { id, name, backend: entry.backend, caps };
    S.done = { id, name, caps };
    S.run.busy = false;
    log(`配置页：已添加「${name}」${caps.live ? "" : "（通话未配置）"}。`);
    paintList();
    paintRun();
    phase("done");
  }
  async function retestLive() {
    const a = state.assistants.find((x) => x.id === S.done?.id);
    if (!a || S.run?.busy) return;
    const step = S.run.steps.find((x) => x.k === "live");
    step.st = "run";
    step.note = "";
    paintRun();
    await wait(900);
    const ready = S.demo !== "noLive";
    step.st = ready ? "ok" : "warn";
    step.note = ready ? T("收到第一段声音", "Got first audio") : T("还是没有实时语音服务。", "Still no realtime voice service.");
    if (ready) { a.caps = { ...capsOf(a), live: true }; S.done.caps.live = true; state.setupResult.caps.live = true; }
    paintList();
    paintRun();
    render();
  }

  // ── 交互
  function onClick(e) {
    const t = (sel) => e.target.closest(sel);
    let n;
    if (t("[data-sw-close]")) return close();
    if ((n = t("[data-sw-view]"))) { S.view = n.dataset.swView; return paintAll(); }
    if ((n = t("[data-sw-lang]"))) { S.lang = n.dataset.swLang; el.querySelector(".sw-lang")?.setAttribute("data-selected", S.lang); return paintAll(); }
    if ((n = t("[data-sw-use]"))) { useAssistant(n.dataset.swUse); render(); return paintList(); }
    if ((n = t("[data-sw-rename]"))) {
      // 和真实页面一样：浏览器自带的输入框，可以打中文；最多 32 个字
      const item = state.assistants.find((x) => x.id === n.dataset.swRename);
      const name = item ? (window.prompt(T("新的名字（最多 32 个字）", "New name (up to 32 characters)"), item.name) || "").trim().slice(0, 32) : "";
      if (item && name && name !== item.name) { item.name = name; log(`配置页：助理改名为「${name}」，设备上马上就是新名字。`); render(); }
      return paintList();
    }
    if ((n = t("[data-sw-remove]"))) {
      const id = n.dataset.swRemove;
      if (S.removing !== id) {
        S.removing = id;
        setTimeout(() => { if (S.removing === id) { S.removing = ""; paintList(); } }, 3000);
      } else { S.removing = ""; removeAssistant(id); render(); }
      return paintList();
    }
    if (t("[data-sw-copy]")) return copyPrompt();
    if ((n = t("[data-sw-chip]"))) { S.name = n.dataset.swChip; S.nameTouched = true; el.querySelector("#sw-name").value = S.name; return; }
    if ((n = t("[data-sw-fill]"))) {
      const live = n.dataset.swFill === "live";
      S.raw = demoReply(live);
      S.demo = live ? "ok" : "noLive";
      S.run = null;
      S.done = null;
      el.querySelector("#sw-raw").value = S.raw;
      el.querySelector("#sw-demo").value = S.demo;
      paintParsed();
      return paintRun();
    }
    if (t("[data-sw-connect]")) return connect();
    if (t("[data-sw-retest]")) return retestLive();
    if (t("[data-sw-again]")) {
      token++;
      Object.assign(S, { raw: "", name: "", nameTouched: false, run: null, done: null });
      phase("opened");
      return paintAll();
    }
  }
  function onInput(e) {
    if (e.target.id === "sw-raw") {
      S.raw = e.target.value;
      if (S.run && !S.run.busy && !S.done) S.run = null;
      paintParsed();
      return paintRun();
    }
    if (e.target.id === "sw-name") { S.name = e.target.value; S.nameTouched = true; }
  }
  function onChange(e) {
    if (e.target.name === "sw-backend") {
      S.backend = e.target.value;
      S.copied = "";
      S.copyFlash = false;
      return paintAll();
    }
    if (e.target.id === "sw-demo") S.demo = e.target.value;
  }

  // ── 开和关
  function open() {
    mount();
    S.open = true;
    el.hidden = false;
    document.body.classList.add("sw-on");
    paintAll();
    if (state.setupPhase === "waiting") phase("opened");
  }
  function close() {
    S.open = false;
    if (el) el.hidden = true;
    document.body.classList.remove("sw-on");
  }
  // 设备上点「取消」或「完成」：丢掉这次配置的所有输入，配对码不留。
  function reset() {
    token++;
    Object.assign(S, { raw: "", name: "", nameTouched: false, copied: "", copyFlash: false, run: null, done: null, removing: "" });
    state.setupPhase = "waiting";
    state.setupNote = "";
    state.setupResult = null;
    close();
  }
  Object.assign(SetupPage, { open, close, reset, isOpen: () => S.open, repaintList: () => el && paintList() });
})();
