// SPDX-License-Identifier: MIT
// Browser + REAL native daemon API, test-only ALSA ABI library and HBC bootstrap double.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn, spawnSync} = require('node:child_process');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
const http = require('node:http');
const root = path.resolve(__dirname, '..');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
(async () => {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'ac-ui-'));
  const state = path.join(temporary, 'state');
  const proof = path.join(root, 'build/proofs'); fs.mkdirSync(proof, {recursive:true});
  const lib = path.join(root, 'build/test-lib'); fs.mkdirSync(lib, {recursive:true});
  let compile = spawnSync('gcc', ['-shared','-fPIC',path.join(root,'tests/fake_alsa.c'),'-o',path.join(lib,'libasound.so.2')]);
  assert.equal(compile.status,0,compile.stderr.toString());
  const child = spawn(path.join(root,'build/native/audiocastd'),['--state-dir',state],
    {env:{...process.env, LD_LIBRARY_PATH:lib},stdio:['ignore','ignore','pipe']});
  let stderr = ''; child.stderr.on('data',d=>stderr+=d.toString());
  let browser;
  try {
    for(let i=0;i<100 && !fs.existsSync(path.join(state,'credentials.json'));i++) await sleep(30);
    const tokens = JSON.parse(fs.readFileSync(path.join(state,'credentials.json')));
    const bootstrap = {...tokens,returnValue:true,control_url:'http://127.0.0.1:7071'};
    const api = (method, endpoint, data) => new Promise((resolve,reject) => {
      const body=data===undefined?'':JSON.stringify(data);
      const request=http.request({host:'127.0.0.1',port:7071,path:endpoint,method,
        headers:{Authorization:'Bearer '+tokens.control_token,'Content-Type':'application/json',
          'Content-Length':Buffer.byteLength(body)},agent:false},response=>{
        let text='';response.on('data',d=>text+=d);response.on('end',()=>{
          try{const result=JSON.parse(text);assert(response.statusCode===200,text);resolve(result);}catch(e){reject(e);}
        });
      });request.on('error',reject);request.setTimeout(3000,()=>request.destroy(Error('API timeout')));request.end(body);
    });
    for(let i=0;i<100;i++){try{await api('GET','/v1/status');break;}catch(error){if(i===99)throw error;await sleep(30);}}
    const options={headless:true,args:['--no-proxy-server']};
    if(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE) options.executablePath=process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE;
    browser = await chromium.launch(options);
    const context=await browser.newContext({viewport:{width:1920,height:1080}});
    await context.addInitScript(credentials=>{
      window.__rootActions=[];
      window.PalmServiceBridge=function(){this.call=(url,payload)=>{
        const command=JSON.parse(payload).command;
        if(url!=='luna://org.webosbrew.hbchannel.service/exec')throw Error('Unexpected Luna service');
        if(!/^sh '\/media\/developer\/apps\/usr\/palm\/applications\/org.webosbrew.audiocast\/service\/startup\/manage.sh' (ensure|enable|disable|stop|status|diagnose)$/.test(command))throw Error('Non-fixed root command');
        const action=command.split(' ').pop();window.__rootActions.push(action);
        setTimeout(()=>this.onservicecallback(JSON.stringify({returnValue:true,stdoutString:action==='ensure'?JSON.stringify(credentials):'TEST: fixed management action accepted'})),0);
      };};
    },bootstrap);
    const page=await context.newPage();const errors=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await page.waitForFunction(()=>document.querySelector('#connection').textContent.includes('connected'));
    assert.equal(await page.locator('#captureState').textContent(),'idle');
    assert.equal(await page.locator('#meterL').evaluate(x=>x.style.width),'0%');
    await page.keyboard.press('ArrowRight');
    assert.equal(await page.evaluate(()=>document.activeElement.dataset.page),'settings');
    await page.keyboard.press('Enter');assert(await page.locator('#settings').isVisible());
    // TV on-screen editor: no physical keyboard required to enter a destination/device.
    await page.locator('#device').focus();await page.keyboard.press('Enter');
    assert(await page.locator('#editor').isVisible());
    for(const char of 'hw:0,13')await page.locator('#keyboard button').filter({hasText:new RegExp('^'+char.replace(/[.*+?^${}()|[\]\\]/g,'\\$&')+'$')}).click();
    await page.locator('#editDone').click();assert.equal(await page.locator('#device').inputValue(),'hw:0,13');
    await page.locator('#confirmed').check();await page.locator('#transport').selectOption('both');
    await page.locator('#configForm button[type=submit]').click();
    await page.waitForFunction(()=>document.querySelector('#message').textContent.includes('Configuration saved'));
    assert((await page.evaluate(()=>window.__rootActions)).includes('disable'));
    // Unknown/unsupported devices must not be auto-verified from inventory.
    assert.equal((await api('GET','/v1/config')).playback_confirmed,true);
    await page.locator('[data-page=dashboard]').click();await page.locator('#start').click();
    await page.waitForFunction(()=>document.querySelector('#captureState').textContent==='capturing');
    await page.waitForFunction(()=>parseFloat(document.querySelector('#meterL').style.width)>0);
    const status=await api('GET','/v1/status');assert(status.levels.rms[0]>0);
    assert.equal(await page.locator('#networkState').innerText(),'Streaming requested');
    await page.screenshot({path:path.join(proof,'dashboard-synthetic-1920.png'),fullPage:true});
    await page.locator('#testReceiver').click();
    await page.waitForFunction(()=>document.querySelector('#message').textContent.includes('no receiver handshake'));
    await page.locator('[data-page=diagnostics]').click();await page.locator('#refreshLogs').click();
    await page.waitForFunction(()=>document.querySelector('#logs').textContent.includes('Capture opened'));
    // Closing the foreground browser must leave the background native engine active.
    await page.close();assert.equal((await api('GET','/v1/status')).state,'capturing');
    await api('POST','/v1/stop',{});
    const next=await context.newPage();await next.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await next.waitForFunction(()=>document.querySelector('#captureState').textContent==='idle');
    await next.locator('[data-page=settings]').click();await next.setViewportSize({width:1280,height:720});
    assert.equal(await next.evaluate(()=>document.documentElement.scrollWidth>window.innerWidth),false);
    await next.keyboard.press('Escape');assert(await next.locator('#dashboard').isVisible());
    await context.close();
    const unsupported=await browser.newContext({viewport:{width:1920,height:1080}});
    const noHbc=await unsupported.newPage();await noHbc.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await noHbc.waitForFunction(()=>document.querySelector('#message').textContent.includes('Homebrew root service unavailable'));
    assert.equal(await noHbc.locator('#meterL').evaluate(x=>x.style.width),'0%');
    await noHbc.screenshot({path:path.join(proof,'no-homebrew-error-1920.png'),fullPage:true});
    assert.deepEqual(errors,[]);
    const result={passed:true,real_native_api:true,alsa_test_double:true,homebrew_test_double:true,
      hardware_tested:false,checks:['idle has zero meters','remote page navigation','onscreen editor',
      'config saves to native daemon','fixed root actions','real captured test PCM meter',
      'honest UDP probe','real log view','app close leaves capture running','1280 no overflow','no Homebrew explicit error'],
      browser_errors:errors};
    fs.writeFileSync(path.join(proof,'ui-results.json'),JSON.stringify(result,null,2)+'\n');console.log(JSON.stringify(result,null,2));
  } finally {
    if(browser)await browser.close();
    child.kill('SIGTERM');await new Promise(resolve=>{if(child.exitCode!==null)return resolve();child.once('exit',resolve);setTimeout(()=>{child.kill('SIGKILL');resolve();},5000).unref();});
    fs.rmSync(temporary,{recursive:true,force:true});
    if(stderr)console.error('Native daemon stderr:',stderr);
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
