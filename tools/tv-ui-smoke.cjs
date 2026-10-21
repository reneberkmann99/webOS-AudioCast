// SPDX-License-Identifier: MIT
// Read-only inspection of a real TV's app via an SSH-forwarded, loopback-only CDP endpoint.
// Never starts capture, opens ALSA, changes routing, or prints authentication credentials.
// Usage: node tools/tv-ui-smoke.cjs [http://127.0.0.1:19998] [proof-directory]
const http = require('node:http');
const path = require('node:path');
const fs = require('node:fs');
const assert = require('node:assert/strict');
const WS = require('../node_modules/playwright-core/lib/utilsBundle').ws;
const endpoint = process.argv[2] || 'http://127.0.0.1:19998';
const out = path.resolve(process.argv[3] || 'build/proofs/tv');
function get(url) {return new Promise((resolve,reject)=>{http.get(url,r=>{let text='';r.on('data',d=>text+=d);r.on('end',()=>{try{resolve(JSON.parse(text));}catch(e){reject(e);}});}).on('error',reject);});}
(async()=>{
  const targets = await get(endpoint+'/json');
  const target = targets.find(x=>x.url.includes('org.webosbrew.audiocast/index.html'));
  assert(target,'AudioCast must be open on the TV');
  const address = new URL(endpoint), uri = new URL(target.webSocketDebuggerUrl);
  uri.host = address.host;
  const socket = new WS(uri.href), pending = new Map();let seq=0;
  await new Promise((resolve,reject)=>{socket.once('open',resolve);socket.once('error',reject);});
  socket.on('message',raw=>{const message=JSON.parse(raw);const call=pending.get(message.id);if(call){pending.delete(message.id);clearTimeout(call.timer);message.error?call.reject(Error(message.error.message)):call.resolve(message.result);}});
  function cmd(method,params={}) {return new Promise((resolve,reject)=>{const id=++seq,timer=setTimeout(()=>{pending.delete(id);reject(Error(method+' timed out'));},10000);pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params}));});}
  async function evaluate(expression) {const reply=await cmd('Runtime.evaluate',{expression,returnByValue:true});if(reply.wasThrown||reply.exceptionDetails)throw Error('TV script failed: '+JSON.stringify(reply));return reply.result.value;}
  try {
    let status;
    for(let i=0;i<35;i++) {
      if(!await evaluate("document.readyState === 'complete' && !!document.getElementById('connection') && !!document.getElementById('autostart')")) {
        await new Promise(r=>setTimeout(r,750)); continue;
      }
      status=JSON.parse(await evaluate(`JSON.stringify({ua:navigator.userAgent,width:innerWidth,height:innerHeight,styles:document.styleSheets.length,color:getComputedStyle(document.body).color,connection:document.getElementById('connection').textContent,source:document.getElementById('sourceWarning').textContent,boot:document.getElementById('bootStatus')&&document.getElementById('bootStatus').textContent,build:document.querySelector('.build')&&document.querySelector('.build').textContent,overflow:document.documentElement.scrollWidth>innerWidth,ipVisible:document.getElementById('destIp').getBoundingClientRect().width>200,portVisible:document.getElementById('destPort').getBoundingClientRect().width>80,startBlocked:document.getElementById('start').disabled,sourceId:document.getElementById('device').value,dileOffered:!!document.querySelector('#device option[value="mstar-upload:0"]'),dileConsentVisible:!!document.getElementById('dileGainRow')&&!document.getElementById('dileGainRow').hidden,dileConsent:!!document.getElementById('dileGainConfirmed')&&document.getElementById('dileGainConfirmed').checked,autostartDisabled:document.getElementById('autostart').disabled,autostart:document.getElementById('autostart').checked})`));
      if(status.connection.includes('online')&&!status.boot.includes('checking'))break;
      await new Promise(r=>setTimeout(r,750));
    }
    assert(status,'AudioCast DOM did not finish loading');
    assert.equal(status.styles,1);assert.equal(status.color,'rgb(237, 243, 255)');
    assert(status.connection.includes('online'),JSON.stringify(status));
    assert(status.boot.includes('starts with the TV'),JSON.stringify(status));
    assert.equal(status.overflow,false);assert(status.ipVisible&&status.portVisible);
    fs.mkdirSync(out,{recursive:true});
    status.hardware_tested=true;status.capture_verified=false;status.ui_only=true;
    if(status.sourceId==='mstar-upload:0') {assert(status.dileOffered&&status.dileConsentVisible);assert(status.autostartDisabled&&!status.autostart);}
    fs.writeFileSync(path.join(out,'ui-status.json'),JSON.stringify(status,null,2)+'\n');
    try {const shot=await cmd('Page.captureScreenshot',{format:'png'});fs.writeFileSync(path.join(out,'tv-gui.png'),Buffer.from(shot.data,'base64'));status.screenshot='tv-gui.png';}
    catch(error){status.screenshot_unavailable=error.message;}
    console.log(JSON.stringify(status,null,2));
  } finally {socket.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
