/* SPDX-License-Identifier: MIT */
(function (global) {
  'use strict';
  var BASE = '/media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh';
  var actions = { ensure: 1, enable: 1, disable: 1, stop: 1, status: 1, diagnose: 1 };
  var pending = [];

  function callExec(command, onDone) {
    var bridge = null, finished = false;
    var timer = global.setTimeout(function () { finish(new Error('Homebrew service timed out. Open Homebrew Channel, then reconnect.')); }, 15000);
    function finish(error, reply) {
      if (finished) return;
      finished = true;
      global.clearTimeout(timer);
      if (bridge) { var at = pending.indexOf(bridge); if (at >= 0) pending.splice(at, 1); }
      onDone(error, reply);
    }
    if (global.webOS && global.webOS.service && global.webOS.service.request) {
      try {
        global.webOS.service.request('luna://org.webosbrew.hbchannel.service', {
          method: 'exec', parameters: { command: command },
          onSuccess: function (reply) { finish(null, reply); },
          onFailure: function (error) { finish(new Error((error && error.errorText) || 'Homebrew exec failed')); }
        });
        return;
      } catch (error) { /* Native PalmServiceBridge is available without the optional webOS.js SDK. */ }
    }
    if (!global.PalmServiceBridge) { finish(new Error('Homebrew root service unavailable. Run on a rooted LG TV with Homebrew Channel.')); return; }
    bridge = new global.PalmServiceBridge();
    pending.push(bridge); // Retain the bridge until its asynchronous native callback completes.
    bridge.onservicecallback = function (text) {
      var reply;
      try { reply = JSON.parse(text); } catch (error) { finish(new Error('Invalid Homebrew response')); return; }
      finish(null, reply);
    };
    try {
      // PalmServiceBridge takes the FULL method URI and a flat method payload, unlike webOS.service.request.
      bridge.call('luna://org.webosbrew.hbchannel.service/exec', JSON.stringify({ command: command }));
    } catch (error) { finish(error); }
  }

  global.AudioCastHomebrew = {
    rootAction: function (action, callback) {
      if (!actions[action]) { callback(new Error('Unsupported action')); return; }
      callExec("sh '" + BASE + "' " + action, function (error, reply) {
        if (error) { callback(error); return; }
        if (!reply || reply.returnValue === false || reply.error || (typeof reply.code === 'number' && reply.code !== 0)) {
          callback(new Error((reply && (reply.errorText || reply.stderrString || reply.error)) || 'Homebrew exec failed')); return;
        }
        var out = reply.stdoutString || '';
        if (action !== 'ensure') { callback(null, out); return; }
        var creds;
        try { creds = JSON.parse(out); } catch (parseError) { callback(new Error('Invalid bootstrap JSON')); return; }
        if (!creds.returnValue || !/^[a-f0-9]{64}$/.test(creds.control_token || '') || !/^[a-f0-9]{64}$/.test(creds.stream_token || '')) {
          callback(new Error('Invalid bootstrap credentials')); return;
        }
        callback(null, creds);
      });
    }
  };
}(window));
