const {chromium}=require('playwright');
const path=require('node:path');
const {pathToFileURL}=require('node:url');
const assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({...(process.env.VALET_CHROME ? {executablePath:process.env.VALET_CHROME} : {}),headless:true});
 try {
 for(const design of ['classic','v2']) {
  const page=await browser.newPage({viewport:{width:1100,height:1050}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(pathToFileURL(path.resolve(__dirname,'../../sim/index.html')).href);
  await page.click(`[data-design="${design}"]`);
  await page.evaluate(()=>{state.screen='settings';state.settingsScreen='talk';state.settingsTab='talk';render()});
  assert.deepEqual(await page.locator('[data-provider]').evaluateAll(x=>x.map(b=>b.dataset.provider)),['baiwen','agent']);
  await page.evaluate(()=>{state.lang='en';render()});
  assert.deepEqual(await page.locator('[data-provider]').evaluateAll(x=>x.map(b=>b.dataset.provider)),['baiwen','agent']);
  await page.selectOption('#edition','global');
  assert.deepEqual(await page.locator('[data-provider]').evaluateAll(x=>x.map(b=>b.dataset.provider)),['yunxin','agent']);
  await page.evaluate(()=>{state.lang='zh';state.settingsScreen='screen';state.settingsTab='screen';render()});
  assert.equal(await page.locator('[data-black-depth]').count(),3);
  await page.locator('[data-black-depth="deep"]').click();
  assert.equal(await page.evaluate(()=>state.blackDepth),'deep');
  await page.evaluate(()=>{state.settingsScreen='diag';state.settingsTab='test';render()});
  await page.locator('[data-diagnostic="auto"]').click();
  assert.match(await page.locator('#panel').innerText(),/蓝牙芯片版本/);
  await page.locator('[data-vcom-query]').click();assert.equal(await page.evaluate(()=>state.vcomRead),true);
  await page.locator('[data-wifi-test]').click();assert.equal(await page.evaluate(()=>state.wifiTest),true);
  await page.evaluate(()=>chin(-1));assert.equal(await page.evaluate(()=>state.wifiTest),false);
  assert.equal(await page.evaluate(()=>state.diagnostic),'auto');
  await page.locator('[data-diagnostic="ui"]').click();assert.equal(await page.evaluate(()=>state.diagnostic),'ui');
  await page.locator('[data-diagnostic-close]').click();
  await page.locator('[data-device-off]').click();assert.equal(await page.evaluate(()=>state.off),true);
  await page.evaluate(()=>powerOn());await page.waitForTimeout(1000);
  assert.equal(await page.evaluate(()=>state.off),false);
  await page.evaluate(()=>{state.screen='settings';state.settingsScreen='diag';state.settingsTab='test';render()});
  await page.locator('[data-device-restart]').click();await page.waitForTimeout(1000);
  assert.equal(await page.evaluate(()=>state.screen),'home');
  await page.evaluate(()=>{state.screen='settings';state.settingsScreen='screen';state.settingsTab='screen';render()});
  assert.equal(await page.locator('[data-black-depth]').count(),0);assert.match(await page.locator('#panel').innerText(),/1200 mV/);
  await page.waitForTimeout(500);
  if(process.env.VALET_SCREENSHOTS) await page.screenshot({path:path.join(process.env.VALET_SCREENSHOTS,`upstream-${design}.png`)});
  assert.deepEqual(errors,[]);await page.close();
 }
 console.log('Upstream prototype: both tabs, regional providers independent of language, VCOM fallback/query, diagnostics/Wi-Fi back, restart/off PASS');
 }finally{await browser.close()}
})().catch(e=>{console.error(e);process.exitCode=1});
