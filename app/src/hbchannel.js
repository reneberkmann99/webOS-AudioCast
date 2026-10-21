/* SPDX-License-Identifier: MIT */
(function (global) {
  'use strict';
  var BASE = "/media/developer/apps/usr/palm/applications/org.webosbrew.audiocast/service/startup/manage.sh";
  var actions = {ensure: true, enable: true, disable: true, stop: true, status: true, diagnose: true};
  var bridges = [];
  function rootAction(action, callback) {
    if (!actions[action]) { callback(new Error('Unsupported local management action')); return; }
    if (!global.PalmServiceBridge) { callback(new Error('Homebrew root service unavailable. Open AudioCast on a rooted LG TV with Homebrew Channel installed.')); return; }
    var bridge = new global.PalmServiceBridge(), done = false;
    bridges.push(bridge);
    var timer = global.setTimeout(function () { finish(new Error('Homebrew service timed out; check root status in Homebrew Channel.')); }, 10000);
    function finish(error, value) {
      if (done) { return; } done = true; global.clearTimeout(timer);
      bridges.splice(bridges.indexOf(bridge), 1); callback(error, value);
    }
    bridge.onservicecallback = function (text) {
      try {
        var reply = JSON.parse(text);
        if (reply.returnValue === false || reply.error || reply.errorCode) { throw new Error(reply.errorText || reply.error || 'Homebrew exec failed'); }
        var out = reply.stdoutString || '';
        if (action === 'ensure') {
          var credentials = JSON.parse(out);
          if (!credentials.returnValue || !/^[a-f0-9]{64}$/.test(credentials.control_token) || !/^[a-f0-9]{64}$/.test(credentials.stream_token)) { throw new Error('Invalid AudioCast bootstrap response'); }
          finish(null, credentials);
        } else { finish(null, out); }
      } catch (error) { finish(error); }
    };
    try {
      // Both command path and action are fixed. Never insert TV/network input into a shell command.
      bridge.call('luna://org.webosbrew.hbchannel.service/exec', JSON.stringify({command: "sh '" + BASE + "' " + action}));
    } catch (error) { finish(error); }
  }
  global.AudioCastHomebrew = {rootAction: rootAction};
}(window));
