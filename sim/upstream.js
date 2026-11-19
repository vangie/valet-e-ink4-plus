// Generated baseline identity is checked by tools/upstream/validate.py.
const UPSTREAM = Object.freeze({
  sourceVersion: "2.0.80",
  sourceCommit: "2e8501095eaf1980be0d6d8b7005d0ffeae505f7",
  releaseVersion: "2.0.81",
  prototypeVersion: "2.0.81",
  prototypeBasis: "release-notes",
});
function providerChoices() {
  // Edition is a hardware/firmware build choice, independent of UI language.
  const official = state.edition === "global" ? "yunxin" : "baiwen";
  return Object.entries(PROVIDERS).filter(([id]) => id === official || id === "agent");
}
function prototypeVersionLabel() {
  return L(`原型 ${UPSTREAM.prototypeVersion} · 源码 ${UPSTREAM.sourceVersion}`,
    `Prototype ${UPSTREAM.prototypeVersion} · source ${UPSTREAM.sourceVersion}`);
}
function calibrationControls() {
  if (state.vcomRead) return `<section class="calibration"><h3>VCOM</h3><p class="note">${L("已读取屏幕校准值", "Panel calibration loaded")} · ${state.vcom} mV</p></section>`;
  return `<section class="calibration"><h3>${L("黑色深度", "Black depth")}</h3>${opts([["light", L("浅", "Light")], ["standard", L("标准", "Standard")], ["deep", L("深", "Deep")]], state.blackDepth, "data-black-depth")}<p class="note">${L("未读取到 VCOM，使用黑色深度档位。", "VCOM unavailable; choose a black depth preset.")}</p></section>`;
}
function diagnosticPane() {
  const entries = [["auto", "自动测试", "Automatic test"], ["ui", "UI 测试", "UI test"],
    ["touch", "触摸测试", "Touch test"], ["battery", "电量测试", "Battery test"],
    ["aging", "老化测试", "Aging test"], ["display", "显示测试", "Display test"],
    ["margin", "边距测试", "Margin test"], ["network", "网络信号", "Network signal"]];
  const actions = entries.map(([id,zh,en]) => `<button class="choice block" data-diagnostic="${id}">${L(zh,en)}</button>`).join("");
  const detail = state.diagnostic ? `<section class="diagnostic-detail"><h3>${esc(entries.find(([id]) => id === state.diagnostic)?.[state.lang === "en" ? 2 : 1] || "")}</h3>
    <p>${L("模拟数据，不执行硬件测试", "Simulated values; no hardware test runs")}</p>
    ${state.diagnostic === "auto" ? `<p>VCOM · ${state.vcomRead ? state.vcom + " mV" : L("未读取", "Unavailable")}</p><p>${L("蓝牙芯片版本", "Bluetooth firmware")} · BH1098 ${state.bluetoothVersion}</p><p>SD · 12.4 / 29.1 GB</p><button class="choice" data-vcom-query>${L("查询 VCOM（模拟）", "Query VCOM (demo)")}</button><button class="choice" data-diagnostic="ui">UI ${L("测试", "test")}</button><button class="choice" data-wifi-test>${L("Wi-Fi 测试", "Wi-Fi test")}</button>` : `<p>${L("使用屏幕与机身按键检查布局，返回后保留设置。", "Check the layout and device keys. Settings remain on return.")}</p>`}
    <button class="choice" data-diagnostic-close>${L("返回测试列表", "Back to tests")}</button></section>` : "";
  const wifi = state.wifiTest ? `<div class="mask"><div class="dialog"><b>Wi-Fi ${L("测试", "test")}</b><p>Metalio-Home · ${L("连接成功（模拟）", "Connected (demo)")}</p><button data-wifi-test-close>${L("关闭并返回测试", "Close and return")}</button></div></div>` : "";
  return `${detail || actions}<h3>${L("设备操作", "Device actions")}</h3><button class="choice block" data-device-restart>${L("重启", "Restart")}</button><button class="choice block" data-device-off>${L("关机", "Power off")}</button>${wifi}`;
}
function closeDiagnosticOverlay() {
  if (state.wifiTest) { state.wifiTest = false; render(); return true; }
  if (state.diagnostic && state.screen === "settings") { state.diagnostic = ""; render(); return true; }
  return false;
}
