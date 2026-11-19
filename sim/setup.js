// 配置页。真机上是设备在局域网里提供的一个网页，手机或电脑打开 http://<设备 IP>/setup。
// 模拟器里把它画成一个浏览器小窗，和设备屏幕同时在页面上，两边的进度是同步的。
// 流程：选助理类型 → 复制提示发给助理 → 把助理的回复整段粘回来 → 保存到设备并连接测试。
// 页面只在设备的「添加助理」界面开着时才有；它不中转语音，也不保存配对码。
const SetupPage = (() => {
  const S = {
    open: false, view: "desktop", lang: null, backend: "openclaw",
    copied: "", raw: "", name: "", demo: "ok", run: null, removing: "", done: null,
  };
  const T = (zh, en) => ((S.lang || state.lang) === "en" ? en : zh);
  const lang = () => ((S.lang || state.lang) === "en" ? "en" : "zh");
  const BACKENDS = { openclaw: "OpenClaw", hermes: "Hermes" };
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

  // ── 设备那一头：配置页做的每一步，设备屏幕都跟着变。
  function phase(next, note = "") {
    state.setupPhase = next;
    state.setupNote = note;
    render();
  }
  function deviceUrl() {
    return `${state.localServiceUrl}/setup`;
  }

  // ── 读助理的回复。助理通常回一段话，里面有一行 JSON、地址、配对码，所以整段粘贴也要认得出来。
  function b64json(code) {
    const raw = code.replace(/-/g, "+").replace(/_/g, "/");
    return JSON.parse(atob(raw + "=".repeat((4 - (raw.length % 4)) % 4)));
  }
  function privateHost(host) {
    return /^(localhost|127\.|10\.|192\.168\.|169\.254\.|172\.(1[6-9]|2\d|3[01])\.|\[?::1\]?$|0\.0\.0\.0)/i.test(host)
      || /\.(local|lan|internal|home|localdomain)$/i.test(host) || !host.includes(".");
  }
  function parse(raw) {
    const text = raw.trim();
    if (!text) return { empty: true, problems: [] };
    const found = (text.match(/\{[^{}]*\}/g) || []).map((s) => { try { return JSON.parse(s); } catch { return null; } })
      .find((o) => o && (o.url || o.gatewayUrl));
    let url = found?.url || found?.gatewayUrl || "";
    let code = found?.code || found?.setupCode || "";
    let name = found?.name || found?.assistantName || "";
    if (!url) url = (text.match(/wss?:\/\/[^\s"'<>，。,]+/i) || [])[0] || "";
    if (!code) code = (text.match(/(?:配对码|setup\s*code|code)\s*[:：=]\s*["']?([A-Za-z0-9_\-+/=]{8,})/i) || [])[1] || "";
    if (!name) name = ((text.match(/(?:助理名称|assistant\s*name)\s*[:：=]\s*([^\n\r]+)/i) || [])[1] || "").trim().replace(/[。.]$/, "");
    const problems = [];
    let host = "";
    let urlCount = 0;
    if (!url) problems.push(T("没找到地址。助理的回复里应该有一个 wss:// 开头的网关地址。", "No address found. The reply should contain a gateway address starting with wss://."));
    else {
      try {
        const u = new URL(url);
        host = u.hostname;
        if (u.protocol === "ws:") problems.push(T("ws:// 是明文连接，设备不会用。请让助理换成证书有效的 wss://。", "ws:// is plain text and the device will not use it. Ask for a wss:// address with a valid certificate."));
        else if (u.protocol !== "wss:") problems.push(T("地址要以 wss:// 开头。", "The address must start with wss://."));
        else if (privateHost(host)) problems.push(T("这是局域网或本机地址。设备可能在用 4G，连不上。请让助理换成公网 wss:// 入口。", "This is a LAN or local address. The device may be on 4G and could not reach it. Ask for a public wss:// entry."));
      } catch { problems.push(T("地址格式不对。", "The address is malformed.")); }
    }
    urlCount = problems.length;
    if (!code) problems.push(T("没找到配对码。", "No pairing code found."));
    else {
      try {
        const inner = b64json(code);
        if (!inner.bootstrapToken) throw new Error();
        if (url && new URL(inner.url).href !== new URL(url).href) problems.push(T("地址和配对码里的地址不一致。请让助理重新生成，不要手改。", "The address differs from the one inside the code. Ask the assistant to generate it again; do not edit by hand."));
      } catch { problems.push(T("配对码读不出来。它应该是助理用配对命令生成的一整串字符，别只复制一半。", "The code cannot be read. It is one long string made by the pairing command; do not copy only part of it.")); }
    }
    const same = state.assistants.find((item) => item.url === url);
    return { empty: false, url, code, name, host, problems, same, ok: problems.length === 0, urlOk: Boolean(url) && urlCount === 0, codeOk: Boolean(code) && problems.length === urlCount };
  }

  // 演示用的助理回复，格式和提示里要求的一样。
  function demoReply(live) {
    const hermes = S.backend === "hermes";
    const url = `wss://${hermes ? "hermes" : "openclaw"}.example.com/valet/v1`;
    const inner = JSON.stringify({ url, bootstrapToken: `demo-${Math.random().toString(36).slice(2, 10)}` });
    const code = btoa(inner).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    const first = lang() === "en"
      ? `Result: plugin ${hermes ? "valet-hermes" : "valet-openclaw"} reused, public entry reused (Cloudflare named tunnel). Ask is ready; Call is ${live ? "ready" : "not configured: the gateway has no realtime voice credential"}. Not tested: real device voice.`
      : `准备结果：插件 ${hermes ? "valet-hermes" : "valet-openclaw"} 已复用，公网入口复用 Cloudflare 命名隧道。提问已就绪；通话${live ? "已就绪" : "尚未配置：网关缺少实时语音凭据"}。未实测：设备实机语音。`;
    return lang() === "en"
      ? `${first}\n{"url":"${url}","code":"${code}"}\nAddress: ${url}\nPairing code: ${code}\nAssistant name: ${hermes ? "Work Hermes" : "Home OpenClaw"}`
      : `${first}\n{"url":"${url}","code":"${code}"}\n地址：${url}\n配对码：${code}\n助理名称：${hermes ? "工作的 Hermes" : "家里的 OpenClaw"}`;
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

  return { S, T, lang, BACKENDS, wait, phase, deviceUrl, parse, demoReply, qr };
})();
