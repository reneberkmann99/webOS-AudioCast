// SPDX-License-Identifier: MIT
// Frontend-only DILE consent fixture. No vendor HAL, real capture, or receiver traffic.
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '..');
(async () => {
  const options = {headless:true,args:['--no-proxy-server']};
  if (process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE) options.executablePath = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE;
  const browser = await chromium.launch(options);
  const errors = [], writes = [], commands = [];
  let config = {backend:'alsa',device:'',destination:'127.0.0.1',destination_port:5004,
    transport:'rtp',rate:48000,channels:2,sample_format:'S16_LE',http_lan:false,
    autostart:false,playback_confirmed:false,dile_gain_confirmed:false};
  let streaming = false;
  const inventory = {soc_mstar_mad:true,devices:[
    {id:'hw:0,0',name:'MStar MAD',safe_to_open:false},
    {id:'mstar-upload:0',name:'MStar vendor playback tap',backend:'dile',experimental:true,safe_to_open:true},
    {id:'hw:2,13',name:'Explicit ALSA fixture',safe_to_open:true,candidate:true}
  ]};
  try {
    const context = await browser.newContext({viewport:{width:1920,height:1080}});
    await context.addInitScript(() => {
      window.PalmServiceBridge = function () {
        this.call = (uri, payload) => {
          if (uri !== 'luna://org.webosbrew.hbchannel.service/exec') throw Error('Wrong Homebrew URI');
          const action = JSON.parse(payload).command.split(' ').pop();
          const value = action === 'ensure' ? JSON.stringify({returnValue:true,control_token:'a'.repeat(64),stream_token:'b'.repeat(64)}) : 'fixture: enabled';
          setTimeout(() => this.onservicecallback(JSON.stringify({returnValue:true,stdoutString:value})), 0);
        };
      };
    });
    await context.route('http://127.0.0.1:7071/**', async route => {
      const request = route.request(), endpoint = new URL(request.url()).pathname;
      let body;
      if (request.method() === 'OPTIONS') body = {};
      else if (endpoint === '/v1/devices') body = inventory;
      else if (endpoint === '/v1/config' && request.method() === 'POST') {
        const update = request.postDataJSON(); writes.push(update); config = {...config,...update}; body = config;
      } else if (endpoint === '/v1/config') body = config;
      else if (endpoint === '/v1/start' || endpoint === '/v1/test-capture') {
        assert.equal(config.backend,'dile'); assert.equal(config.dile_gain_confirmed,true);
        assert.equal(config.playback_confirmed,true); assert.equal(config.autostart,false);
        commands.push(endpoint); streaming = true; body = {ok:true,note:'Fixture only; no capture'};
      } else if (endpoint === '/v1/stop') {streaming = false; body = {ok:true};}
      else if (endpoint === '/v1/status') body = {state:streaming?'capturing':'idle',streaming,test_active:false,
        signal:'unknown',config,format:{rate:48000,channels:2,sample_format:'S16_LE'},levels:{rms:[],peak:[]},transport:'rtp'};
      else throw Error('Unexpected fixture request '+endpoint);
      await route.fulfill({status:200,contentType:'application/json',headers:{'Access-Control-Allow-Origin':'*',
        'Access-Control-Allow-Headers':'Authorization, Content-Type','Access-Control-Allow-Methods':'GET, POST, OPTIONS'},body:JSON.stringify(body)});
    });
    const page = await context.newPage(); page.on('pageerror', e => errors.push(e.message));
    await page.goto(pathToFileURL(path.join(root,'app/index.html')).href);
    await page.waitForFunction(() => document.querySelector('#device').options.length === 4);
    assert.equal(await page.locator('#device').inputValue(),'','Never auto-select a vendor source');
    assert(await page.locator('#start').isDisabled()); assert(await page.locator('#testCapture').isDisabled());
    assert(await page.locator('#dileGainRow').isHidden());
    assert(await page.locator('#device option[value="hw:0,0"]').isDisabled());
    await page.locator('#device').selectOption('mstar-upload:0');
    assert(await page.locator('#dileGainRow').isVisible());
    assert(await page.locator('#start').isDisabled()); assert(await page.locator('#testCapture').isDisabled());
    assert(await page.locator('#autostart').isDisabled());
    for (const id of ['rate','channels','sampleFormat']) assert(await page.locator('#'+id).isDisabled());
    await page.locator('#confirmed').check();
    assert(await page.locator('#start').isDisabled(),'Playback confirmation is not upload-gain consent');
    await page.locator('#dileGainConfirmed').check();
    assert.equal(await page.locator('#start').isDisabled(),false);
    assert.equal(await page.locator('#testCapture').isDisabled(),false);
    await page.waitForTimeout(1100);
    assert(await page.locator('#dileGainConfirmed').isChecked(),'Polling preserves unsaved consent');
    await page.locator('#applyDest').click();
    await page.waitForFunction(() => document.querySelector('#message').textContent.includes('Configuration saved'));
    assert.equal(writes.at(-1).backend,'dile'); assert.equal(writes.at(-1).dile_gain_confirmed,true);
    assert.equal(writes.at(-1).rate,48000); assert.equal(writes.at(-1).channels,2);
    assert.equal(writes.at(-1).sample_format,'S16_LE'); assert.equal(writes.at(-1).autostart,false);
    assert((await page.locator('#sourceWarning').innerText()).includes('30 seconds'));
    const proof = path.join(root,'build/proofs'); fs.mkdirSync(proof,{recursive:true});
    for (const [width,height] of [[1280,720],[1920,1080]]) {
      await page.setViewportSize({width,height});
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),false);
      await page.screenshot({path:path.join(proof,'dile-consent-fixture-'+width+'.png'),fullPage:true});
    }
    await page.locator('#dileGainConfirmed').uncheck();
    assert(await page.locator('#start').isDisabled()); assert(await page.locator('#testCapture').isDisabled());
    await page.locator('#dileGainConfirmed').check(); await page.locator('#testCapture').click();
    await page.waitForFunction(() => document.querySelector('#streamState').textContent === 'CAPTURING');
    assert.deepEqual(commands,['/v1/test-capture']);
    assert(await page.locator('#dileGainConfirmed').isDisabled());
    assert(await page.locator('#applyDest').isDisabled());
    await page.locator('#stop').click();
    await page.waitForFunction(() => !document.querySelector('#device').disabled);
    await page.locator('#device').selectOption('hw:2,13');
    assert(await page.locator('#dileGainRow').isHidden());
    assert.equal(await page.locator('#dileGainConfirmed').isChecked(),false);
    assert.equal(await page.locator('#confirmed').isChecked(),false);
    assert.equal(await page.locator('#rate').isDisabled(),false);
    await page.locator('#applyDest').click();
    await page.waitForFunction(() => document.querySelector('#message').textContent.includes('Configuration saved'));
    assert.equal(writes.at(-1).backend,'alsa'); assert.equal(writes.at(-1).dile_gain_confirmed,false);
    assert.deepEqual(errors,[]);
    console.log(JSON.stringify({passed:true,frontend_fixture_only:true,actual_capture:false,
      checks:['explicit vendor source selection','unsafe MAD remains blocked','separate upload-gain consent',
      'playback confirmation required','fixed DILE format','DILE autostart disabled','polling preserves edits',
      'backend and consent included in config','capture controls locked while active','switching source clears consent',
      '1280 and 1920 no horizontal overflow'],browser_errors:errors},null,2));
  } finally {await browser.close();}
})().catch(error => {console.error(error);process.exitCode=1;});
