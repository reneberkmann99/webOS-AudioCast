// SPDX-License-Identifier: MIT
// Real native API + real RTP sockets; synthetic ALSA and Homebrew explicitly doubled.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn, spawnSync} = require('node:child_process');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
const http = require('node:http');
const dgram = require('node:dgram');
const root = path.resolve(__dirname, '..');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
(async () => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'ac-ui-')), state = path.join(temp, 'state');
  const proof = path.join(root, 'build/proofs'); fs.mkdirSync(proof, {recursive:true});
  const lib = path.join(root, 'build/test-lib'); fs.mkdirSync(lib, {recursive:true});
  const pcm = path.join(temp, 'pcm'); fs.writeFileSync(pcm, '00-13: CAPTURE_TEST : CAPTURE_TEST : capture 1\n');
  const compile = spawnSync('gcc', ['-shared','-fPIC',path.join(root,'tests/fake_alsa.c'),'-o',path.join(lib,'libasound.so.2')]);
  assert.equal(compile.status,0,compile.stderr.toString());
  const child = spawn(path.join(root,'build/native/audiocastd'),['--state-dir',state],
    {env:{...process.env,LD_LIBRARY_PATH:lib,AUDIOCAST_PROC_PCM:pcm},stdio:['ignore','ignore','pipe']});
  let stderr = ''; child.stderr.on('data',d=>stderr+=d);
  let browser, packets = 0; const udp = dgram.createSocket('udp4'); udp.on('message',()=>packets++);
  await new Promise(resolve=>udp.bind(15004,'127.0.0.1',resolve));
  try {
    for(let i=0;i<100&&!fs.existsSync(path.join(state,'credentials.json'));i++)await sleep(30);
    const tokens=JSON.parse(fs.readFileSync(path.join(state,'credentials.json'))), bootstrap={...tokens,returnValue:true,control_url:'http://127.0.0.1:7071'};
    const api=(method,endpoint,data)=>new Promise((resolve,reject)=>{
      const body=data===undefined?'':JSON.stringify(data), req=http.request({host:'127.0.0.1',port:7071,path:endpoint,method,
        headers:{Authorization:'Bearer '+tokens.control_token,'Content-Type':'application/json','Content-Length':Buffer.byteLength(body)},agent:false},res=>{
        let text='';res.on('data',d=>text+=d);res.on('end',()=>{try{const result=JSON.parse(text);assert.equal(res.statusCode,200,text);resolve(result);}catch(e){reject(e);}});
      });req.on('error',reject);req.setTimeout(3000,()=>req.destroy(Error('API timeout')));req.end(body);
    });
    for(let i=0;i<100;i++){try{await api('GET','/v1/status');break;}catch(e){if(i===99)throw e;await sleep(30);}}
    const options={headless:true,args:['--no-proxy-server']};
    if(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE)options.executablePath=process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE;
    browser=await chromium.launch(options);
    const context=await browser.newContext({viewport:{width:1920,height:1080}});
    await context.addInitScript(credentials=>{
      window.__rootActions=[];
      window.PalmServiceBridge=function(){this.call=(url,payload)=>{
        if(url!=='luna://org.webosbrew.hbchannel.service/exec')throw Error('Bridge needs full method URI');
        const params=JSON.parse(payload);if(params.method||params.parameters)throw Error('Bridge payload must be flat');
        const command=params.command;
        if(!/^sh '\/media\/developer\/apps\/usr\/palm\/applications\/org.webosbrew.audiocast\/service\/startup\/manage.sh' (ensure|enable|disable|stop|status|diagnose)$/.test(command))throw Error('Non-fixed root command');
        const action=command.split(' ').pop();window.__rootActions.push(action);
        setTimeout(()=>this.onservicecallback(JSON.stringify({returnValue:true,stdoutString:action==='ensure'?JSON.stringify(credentials):'TEST: action accepted'})),0);
      };};
    },bootstrap);
    const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await page.waitForFunction(()=>document.querySelector('#connection').textContent.includes('online'));
    await page.waitForFunction(()=>document.querySelector('#device').options.length===2);
    assert.equal(await page.evaluate(()=>document.styleSheets.length),1,'Packaged file CSS must load');
    assert.equal(await page.locator('body').evaluate(x=>getComputedStyle(x).color),'rgb(237, 243, 255)');
    assert.equal(await page.locator('#start').isDisabled(),true,'No implicit source confirmation');
    assert.equal(await page.locator('#meterL').evaluate(x=>x.style.width),'0%');
    assert((await page.evaluate(()=>window.__rootActions)).includes('enable'));
    // Keyboard is D-pad navigable, traps focus, and restores it on Done/Back.
    await page.locator('#destIp').click();await page.locator('#kbClear').click();
    await page.locator('#keyboard button').first().focus();await page.keyboard.press('ArrowRight');
    assert.equal(await page.evaluate(()=>document.activeElement.textContent),'2');
    for(const char of '127.0.0.1')await page.locator('#keyboard button').filter({hasText:new RegExp('^'+char.replace(/[.*+?^${}()|[\]\\]/g,'\\$&')+'$')}).click();
    await page.locator('#kbDone').click();assert.equal(await page.locator('#destIp').inputValue(),'127.0.0.1');
    assert.equal(await page.evaluate(()=>document.activeElement.id),'destIp');
    await page.locator('#destPort').fill('15004');await page.locator('#destPort').dispatchEvent('input');
    await sleep(1600);assert.equal(await page.locator('#destPort').inputValue(),'15004','Polling must not overwrite unsaved edits');
    await page.locator('#destPort').click();await page.locator('#kbClear').click();await page.keyboard.press('Escape');
    assert.equal(await page.locator('#destPort').inputValue(),'15004','Back cancels editor');
    await page.locator('#destPort').fill('1');await page.locator('#applyDest').click();
    await page.waitForFunction(()=>document.querySelector('#toast').textContent.includes('1024'));
    assert.equal((await api('GET','/v1/config')).destination_port,5004);
    await page.locator('#destPort').fill('15004');await page.locator('#applyDest').click();
    await page.waitForFunction(()=>document.querySelector('#message').textContent.includes('Configuration saved'));
    assert.equal((await api('GET','/v1/config')).destination_port,15004);
    assert(!(await page.evaluate(()=>window.__rootActions)).includes('disable'),'Manual streaming must not disable service boot startup');
    await page.locator('#device').selectOption('hw:0,13');
    await page.locator('#confirmed').check();await page.locator('#applyDest').click();
    await page.waitForFunction(()=>!document.querySelector('#start').disabled);
    assert.equal((await api('GET','/v1/config')).playback_confirmed,true);
    await page.screenshot({path:path.join(proof,'gui-ready-1920.png'),fullPage:true});
    await page.locator('#start').click();
    await page.waitForFunction(()=>parseFloat(document.querySelector('#meterL').style.width)>0);
    for(let i=0;i<40&&packets===0;i++)await sleep(30);assert(packets>0,'Real RTP packets must arrive at receiver');
    assert.equal((await api('GET','/v1/status')).state,'capturing');
    assert.equal(await page.locator('#applyDest').isDisabled(),true);
    await page.screenshot({path:path.join(proof,'dashboard-synthetic-1920.png'),fullPage:true});
    await page.close();assert.equal((await api('GET','/v1/status')).state,'capturing','Foreground app does not own capture lifetime');
    await api('POST','/v1/stop',{});
    const next=await context.newPage();await next.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await next.waitForFunction(()=>document.querySelector('#connection').textContent.includes('online'));
    await next.locator('#diagnosticsToggle').click();await next.locator('#refreshLogs').click();
    await next.waitForFunction(()=>document.querySelector('#logs').textContent.includes('Capture opened'));
    await next.locator('#exportDiag').click();await next.waitForFunction(()=>document.querySelector('#diagOutput').textContent.includes('privacy'));
    assert(!(await next.locator('#diagOutput').innerText()).includes(tokens.control_token));
    await next.keyboard.press('Escape');assert(await next.locator('#diagnosticsPanel').isHidden());
    for(const [width,height] of [[1280,720],[1920,1080]]){
      await next.setViewportSize({width,height});assert.equal(await next.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
      await next.screenshot({path:path.join(proof,`gui-${width}.png`),fullPage:true});
    }
    // Real discovery guard is exercised; no hazardous device is ever opened.
    fs.writeFileSync(pcm,'00-00: mad_pcm : MStar MAD : capture 1\n00-01: mad_pcm : MStar MAD : capture 1\n');
    await next.locator('#refreshDevices').click();
    await next.waitForFunction(()=>document.querySelector('#sourceWarning').textContent.includes('hang the kernel'));
    assert(await next.locator('#device').isDisabled());assert(await next.locator('#start').isDisabled());assert(await next.locator('#testCapture').isDisabled());
    assert(await next.locator('#autostart').isDisabled());assert.equal(await next.locator('#applyDest').isDisabled(),false,'Destination remains editable');
    await next.screenshot({path:path.join(proof,'mstar-capture-blocked-1920.png'),fullPage:true});
    await context.close();
    // Both native bridge and optional SDK paths have correct, distinct calling conventions.
    const sdk=await browser.newContext();
    await sdk.addInitScript(credentials=>{window.webOS={service:{request:(url,o)=>{
      if(url!=='luna://org.webosbrew.hbchannel.service'||o.method!=='exec'||!o.parameters.command)throw Error('Incorrect SDK call');
      const action=o.parameters.command.split(' ').pop();setTimeout(()=>o.onSuccess({returnValue:true,stdoutString:action==='ensure'?JSON.stringify(credentials):'enabled'}),0);
    }}};},bootstrap);
    const sdkPage=await sdk.newPage();await sdkPage.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await sdkPage.waitForFunction(()=>document.querySelector('#connection').textContent.includes('online'));await sdk.close();
    const unsupported=await browser.newContext({viewport:{width:1920,height:1080}}),noHbc=await unsupported.newPage();
    await noHbc.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await noHbc.waitForFunction(()=>document.querySelector('#message').textContent.includes('Homebrew root service unavailable'));
    assert(await noHbc.locator('#start').isDisabled());assert.equal(await noHbc.locator('#meterL').evaluate(x=>x.style.width),'0%');
    await noHbc.screenshot({path:path.join(proof,'no-homebrew-error-1920.png'),fullPage:true});
    assert.deepEqual(errors,[]);
    const result={passed:true,real_native_api:true,real_rtp_packets:packets,alsa_test_double:true,homebrew_test_double:true,hardware_tested:false,
      checks:['file URI CSS/scripts load','visible styled text','correct native bridge URI/payload','correct webOS SDK request','service boot enable independent of stream autostart','D-pad editor navigation','editor cancel and focus restore','polling preserves unsaved edits','invalid port rejected','real native config save','explicit source confirmation','actual PCM meters and RTP reception','app close leaves capture running','diagnostics without credentials','1280 and 1920 no overflow','MStar unsafe sources blocked','no Homebrew explicit retryable error'],browser_errors:errors};
    fs.writeFileSync(path.join(proof,'ui-results.json'),JSON.stringify(result,null,2)+'\n');console.log(JSON.stringify(result,null,2));
  } finally {
    udp.close();if(browser)await browser.close();child.kill('SIGTERM');
    await new Promise(resolve=>{if(child.exitCode!==null)return resolve();child.once('exit',resolve);setTimeout(()=>{child.kill('SIGKILL');resolve();},5000).unref();});
    fs.rmSync(temp,{recursive:true,force:true});if(stderr)console.error(stderr);
  }
})().catch(e=>{console.error(e);process.exitCode=1;});
