// 配置页。真机上是设备在局域网里提供的一个网页，手机或电脑打开 http://<设备 IP>/setup。
// 模拟器里画一个浏览器小窗，里面嵌的就是设备上那一个页面（sim/valet_setup.html；
// main/valet_setup/valet_setup.html 是指过来的链接），不另画一份。页面通过 window.ValetSetupApi 读写这里的助理列表，
// 字段和设备的 /api/assistants（main/valet_setup/server.cc）一样，所以两边的进度同步。
const SetupPage = (() => {
  const S = { open: false, view: "desktop" };
  const T = (zh, en) => (state.lang === "en" ? en : zh);
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  let el = null;

  // ── 设备那一头：配置页做的每一步，设备屏幕都跟着变。
  function phase(next, note = "") {
    state.setupPhase = next;
    state.setupNote = note;
    render();
  }
  function deviceUrl() {
    return `${state.localServiceUrl}/setup`;
  }

  // 一个看起来像二维码的图案，只画样子，扫不出来。三个角是定位点，其余按地址生成。
  function qr(seed, size) {
    let h = 2166136261;
    for (const ch of seed) h = Math.imul(h ^ ch.charCodeAt(0), 16777619);
    const n = 25;
    const corners = [[0, 0], [n - 7, 0], [0, n - 7]];
    const finder = (x, y) => corners.find(([cx, cy]) => x >= cx && x < cx + 7 && y >= cy && y < cy + 7);
    let cells = "";
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
      let on;
      const c = finder(x, y);
      if (c) {
        const lx = x - c[0], ly = y - c[1];
        on = lx === 0 || lx === 6 || ly === 0 || ly === 6 || (lx >= 2 && lx <= 4 && ly >= 2 && ly <= 4);
      } else if ((x < 8 && y < 8) || (x >= n - 8 && y < 8) || (x < 8 && y >= n - 8)) on = false;
      else { h = Math.imul(h ^ (h >>> 13), 1274126177) >>> 0; on = h % 100 < 46; }
      if (on) cells += `<rect x="${x}" y="${y}" width="1" height="1"/>`;
    }
    return `<svg class="pair-qr" viewBox="-1 -1 ${n + 2} ${n + 2}" width="${size}" height="${size}" shape-rendering="crispEdges" role="img" aria-label="QR"><rect x="-1" y="-1" width="${n + 2}" height="${n + 2}" fill="#fff"/><g fill="#111">${cells}</g></svg>`;
  }

  // ── 浏览器小窗
  function mount() {
    if (el) return;
    el = document.createElement("aside");
    el.className = "sw";
    el.hidden = true;
    el.setAttribute("role", "dialog");
    el.innerHTML = `<div class="sw-chrome"><span class="sw-lights"><i></i><i></i><i></i></span><span class="sw-url"><b></b></span><span class="sw-view"></span><button class="sw-x" data-sw-close aria-label="close">×</button></div><iframe class="sw-frame" title="Valet 连接助理"></iframe>`;
    document.body.appendChild(el);
    el.addEventListener("click", (e) => {
      const view = e.target.closest("[data-sw-view]");
      if (view) { S.view = view.dataset.swView; return paint(); }
      if (e.target.closest("[data-sw-close]")) { close(); render(); }
    });
  }
  function paint() {
    if (!el) return;
    el.setAttribute("aria-label", T("连接助理", "Connect an assistant"));
    el.querySelector(".sw-url b").textContent = deviceUrl();
    el.querySelector(".sw-view").innerHTML = [["desktop", T("电脑", "Computer")], ["phone", T("手机", "Phone")]]
      .map(([v, name]) => `<button data-sw-view="${v}" class="${S.view === v ? "is-on" : ""}">${name}</button>`).join("");
    el.classList.toggle("phone", S.view === "phone");
    const frame = el.querySelector(".sw-frame");
    if (!frame.getAttribute("src")) frame.src = S.hash ? `valet_setup.html?to=${S.hash}#sim` : "valet_setup.html#sim";
  }
  function open(hash = "") {
    S.hash = hash;
    mount();
    S.open = true;
    el.hidden = false;
    document.body.classList.add("sw-on");
    paint();
    if (state.setupPhase === "waiting") phase("opened");
  }
  function close() {
    S.open = false;
    if (!el) return;
    el.hidden = true;
    el.querySelector(".sw-frame").removeAttribute("src");   // 下次打开是新的一页
    document.body.classList.remove("sw-on");
  }
  // 设备上点「取消」或「完成」：这次配置结束，页面关掉。
  function reset() {
    state.setupPhase = "waiting";
    state.setupNote = "";
    state.setupResult = null;
    close();
  }
  // 设备上改了助理（使用、移除）：开着的页面跟着刷新
  function repaintList() {
    const page = el?.querySelector(".sw-frame")?.contentWindow;
    if (page && typeof page.load === "function") page.load().catch(() => {});
  }

  // ── 设备的 /api/assistants（假的）
  let apiNote = "", apiPaired = true;
  const reply = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
  // ── 设备的 /api/libraries（假的）：书库订阅，和设置里「书库订阅」是同一份 state.libs。
  // 真机上订阅时设备会先打开链接：书斋带 Accept: application/json 能拿到名字和家人，其余按 OPDS 读标题。
  async function libraries(opts) {
    await wait(opts.method === "POST" ? 900 : 120);
    if (!opts.method || opts.method === "GET") {
      return reply({ items: state.libs.map((id) => LIB_SOURCES[id]).map((s) => ({ id: s.id, name: s.short, member: s.rich ? "杜万" : "", url: s.url, rich: !!s.rich })) });
    }
    const d = JSON.parse(opts.body || "{}");
    if (d.action === "remove") {
      state.libs = state.libs.filter((x) => x !== d.id);
      log(`配置页：移除书库「${LIB_SOURCES[d.id]?.short || d.id}」。`);
      render();
      return reply({ ok: true });
    }
    if (d.action !== "add") return reply({ error: "设备不认识这个操作" }, 400);
    let id;
    try { const u = new URL(d.url); id = /gutenberg/.test(u.host) ? "gutenberg" : /\/opds\/[a-z0-9]{6}$/i.test(u.pathname) ? "shuzhai" : ""; } catch { id = ""; }
    if (!id) return reply({ error: "设备打开了这个链接，但不是 OPDS 书库。检查一下是不是复制全了。" }, 400);
    if (state.libs.includes(id)) return reply({ error: `已经订过「${LIB_SOURCES[id].short}」了` }, 400);
    state.libs.push(id);
    log(`配置页：订阅「${LIB_SOURCES[id].name}」。`);
    render();
    return reply({ ok: true, id, name: LIB_SOURCES[id].short });
  }
  function repaintLibraries() {
    const page = el?.querySelector(".sw-frame")?.contentWindow;
    if (page && typeof page.libLoad === "function") page.libLoad().catch(() => {});
  }

  window.ValetSetupApi = async (url, opts = {}) => {
    if (url === "/api/libraries") return libraries(opts);
    await wait(120);
    if (!opts.method || opts.method === "GET") {
      return reply({ active: state.activeAssistant || "", saving: false, note: apiNote, paired: apiPaired, transport: "passport-v1",
        items: state.assistants.map((a) => ({ id: a.id, name: a.name, backend: a.backend, url: a.url, paired: a.code !== "" })) });
    }
    const d = JSON.parse(opts.body || "{}");
    const find = (id) => state.assistants.find((a) => a.id === id);
    if (d.action === "select" && find(d.id)) { useAssistant(d.id); apiNote = ""; log(`配置页：改用「${find(d.id).name}」。`); }
    else if (d.action === "rename" && find(d.id)) { find(d.id).name = String(d.name).slice(0, 32); log(`配置页：改名为「${d.name}」。`); }
    else if (d.action === "remove" && find(d.id)) { const n = find(d.id).name; removeAssistant(d.id); apiNote = ""; log(`配置页：移除「${n}」。`); }
    else if (d.action === "add") {
      if (!/^wss?:\/\//.test(d.url || "") || !d.code) return reply({ error: "配对信息不对：要有 wss:// 地址和配对码" }, 400);
      const id = `assistant-${Date.now()}`;
      state.assistants.push({ id, name: String(d.name).slice(0, 32), backend: d.backend === "hermes" ? "Hermes" : "OpenClaw", url: d.url, code: "", caps: { ask: true, live: true, ctx: true } });
      useAssistant(id);
      state.paired = false;
      apiPaired = false;
      apiNote = "请在助理那边批准这台设备。批准后这一页会自己变成「已连接」。";
      phase("connecting", T("等助理批准", "Waiting for approval"));
      log(`配置页：保存「${d.name}」，等助理批准（模拟 4 秒后批准）。`);
      setTimeout(() => {
        const a = find(id);
        if (!a) return;
        a.code = "device-token";
        state.paired = true;
        apiPaired = true;
        apiNote = "已连接。可以在设备上关闭配置页了。";
        state.setupResult = { id, name: a.name, backend: a.backend, caps: a.caps };
        phase("done");
        log(`配置页：「${a.name}」已连接。`);
      }, 4000);
    } else return reply({ error: "设备不认识这个操作" }, 400);
    render();
    return reply({ ok: true });
  };

  return { open, close, reset, isOpen: () => S.open, repaintList, repaintLibraries, qr, deviceUrl };
})();
