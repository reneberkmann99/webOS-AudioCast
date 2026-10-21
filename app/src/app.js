/* SPDX-License-Identifier: MIT */
(function () {
  'use strict';
  var credentials = null, timer = null, polling = false, page = 'dashboard', lastConfig = null;
  var editing = null, editText = '', editFocus = null, pendingBoot = null;
  function el(id) { return document.getElementById(id); }
  function text(id, value) { el(id).textContent = value; }
  function notify(message, error) { text('message', message || ''); el('message').className = error ? 'error' : 'success'; }
  function db(value) { return typeof value === 'number' && value > 0 ? (20 * Math.log(value) / Math.LN10).toFixed(1) + ' dBFS' : '−∞ dBFS'; }
  function api(method, path, body, callback) {
    if (!credentials) { callback(new Error('Service not connected')); return; }
    var xhr = new XMLHttpRequest();
    xhr.open(method, 'http://127.0.0.1:7071' + path, true); xhr.timeout = 4000;
    xhr.setRequestHeader('Authorization', 'Bearer ' + credentials.control_token);
    if (body !== null) { xhr.setRequestHeader('Content-Type', 'application/json'); }
    xhr.onload = function () {
      var result;
      try { result = JSON.parse(xhr.responseText); } catch (error) { callback(new Error('Invalid daemon response')); return; }
      if (xhr.status < 200 || xhr.status >= 300) { callback(new Error(result.error || 'Service HTTP ' + xhr.status)); return; }
      callback(null, result);
    };
    xhr.onerror = function () { callback(new Error('Background service unreachable. Check root access, native ABI, and daemon logs.')); };
    xhr.ontimeout = function () { callback(new Error('Background service response timed out')); };
    xhr.send(body === null ? null : JSON.stringify(body));
  }
  function resetMeter() { el('meterL').style.width = '0%'; el('meterR').style.width = '0%'; text('levelL', '—'); text('levelR', '—'); }
  function render(status) {
    var fmt = status.format || {}, levels = status.levels || {}, rms = levels.rms || [], peak = levels.peak || [];
    var fresh = typeof status.capture_sample_age_ms === 'number' && status.capture_sample_age_ms < 500;
    text('connection', '● Background service connected');
    text('captureState', status.state || 'unknown');
    text('signalState', (status.test_active ? 'Capture-only test · no network transmission. ' : '') + 'Signal: ' + (status.state === 'capturing' && !fresh ? 'stale / waiting for new PCM' : (status.signal || 'unknown')) + (status.silence_seconds >= 5 ? ' · silence for ' + status.silence_seconds.toFixed(0) + ' sec (check known playback/source/mute)' : ''));
    text('sourceName', (status.backend || 'unknown') + ' / ' + (status.device || 'none selected'));
    text('networkState', status.streaming ? 'Streaming requested' : 'Not streaming');
    text('destination', status.transport === 'http' ? 'HTTP :7070' : (status.transport || 'unknown').toUpperCase() + ' → ' + (status.destination || 'unknown'));
    text('receivers', 'HTTP receivers: ' + status.http_clients + ' · RTP receivers: unknown');
    text('throughput', typeof status.throughput_bps === 'number' ? (status.throughput_bps / 1000000).toFixed(2) + ' Mbit/s sent' : 'Throughput unknown');
    text('format', (fmt.sample_format || 'unknown') + ' · ' + (fmt.rate || '?') + ' Hz · ' + (fmt.channels || '?') + ' ch');
    text('tvOutput', 'TV output: ' + (status.audio_output || 'unknown'));
    text('verification', status.source_verified ? 'Playback source: user-confirmed (not hardware qualification)' : 'Playback source: not confirmed');
    text('queueLatency', 'Capture read → sender: ' + (typeof status.capture_to_network_ms === 'number' ? status.capture_to_network_ms.toFixed(1) + ' ms' : 'unknown'));
    text('hardwareLatency', 'ALSA pending: ' + (typeof status.alsa_pending_ms === 'number' ? status.alsa_pending_ms.toFixed(1) + ' ms' : 'unknown'));
    text('resources', 'CPU: ' + (typeof status.cpu_percent === 'number' ? status.cpu_percent.toFixed(1) + '%' : 'unknown') + ' · RAM: ' + (typeof status.rss_kb === 'number' ? (status.rss_kb / 1024).toFixed(1) + ' MiB' : 'unknown'));
    text('lastError', 'Last error: ' + (status.last_error || 'none reported'));
    if (status.state === 'capturing' && fresh) {
      el('meterL').style.width = Math.min(100, Math.max(0, (rms[0] || 0) * 100)) + '%';
      el('meterR').style.width = Math.min(100, Math.max(0, (rms[1] || 0) * 100)) + '%';
      text('levelL', db(peak[0])); text('levelR', fmt.channels === 1 ? 'Mono' : db(peak[1]));
    } else { resetMeter(); }
    el('start').disabled = !!status.streaming || !!status.test_active;
    if (!lastConfig && status.config) { loadConfig(status.config); }
  }
  function poll() {
    if (polling) { return; } polling = true;
    api('GET', '/v1/status', null, function (error, status) {
      polling = false;
      if (error) { text('connection', '○ Service disconnected'); resetMeter(); text('captureState', 'Unknown'); text('signalState', error.message); }
      else { render(status); }
      timer = window.setTimeout(poll, 400);
    });
  }
  function bootstrap() {
    if (timer) { window.clearTimeout(timer); timer = null; }
    text('connection', 'Connecting to Homebrew…');
    window.AudioCastHomebrew.rootAction('ensure', function (error, result) {
      if (error) { credentials = null; notify(error.message, true); text('connection', '○ Homebrew unavailable'); resetMeter(); return; }
      credentials = result; lastConfig = null;
      notify('Service started independently of the TV app. Capture remains off unless you previously enabled autostart.');
      loadDevices();
      if (!polling) { poll(); }
    });
  }
  function showPage(name) {
    page = name;
    var pages = document.querySelectorAll('.page'), buttons = document.querySelectorAll('[data-page]'), i;
    for (i = 0; i < pages.length; i++) { pages[i].hidden = pages[i].id !== name; }
    for (i = 0; i < buttons.length; i++) { buttons[i].className = buttons[i].getAttribute('data-page') === name ? 'selected' : ''; }
    el('credential').hidden = true; text('credential', '');
    if (name === 'diagnostics') { loadLogs(); }
  }
  function loadConfig(config) {
    lastConfig = config;
    var map = {backend: 'backend', device: 'device', transport: 'transport', destinationInput: 'destination', destinationPort: 'destination_port', sampleFormat: 'sample_format', rate: 'rate', channels: 'channels', packetMs: 'packet_ms', profile: 'profile'}, key;
    for (key in map) { if (Object.prototype.hasOwnProperty.call(map, key)) { el(key).value = config[map[key]]; } }
    el('httpLan').checked = config.http_lan; el('confirmed').checked = config.playback_confirmed; el('autostart').checked = config.autostart;
  }
  function loadDevices() {
    api('GET', '/v1/devices', null, function (error, result) {
      if (error) { notify(error.message, true); return; }
      var list = el('deviceList'), i; list.textContent = '';
      var none = document.createElement('option'); none.value = ''; none.textContent = 'Select explicit capture device…'; list.appendChild(none);
      for (i = 0; i < result.devices.length; i++) {
        var device = result.devices[i], option = document.createElement('option'); option.value = device.id;
        option.textContent = device.id + ' · ' + device.name + (device.candidate ? ' · candidate, NOT verified' : ' · unverified'); list.appendChild(option);
      }
      if (!result.devices.length) { notify('No ALSA capture devices inventoried. Check diagnostics; never substitute a microphone.', true); }
    });
  }
  function action(path) {
    api('POST', path, {}, function (error, result) { notify(error ? error.message : (result.message || result.note || 'Command accepted. Check live status for actual readiness.'), !!error); });
  }
  function save(event) {
    event.preventDefault();
    var port = el('destinationPort').value;
    if (!/^[0-9]+$/.test(port)) { notify('Destination port must be a number.', true); return; }
    var config = {backend: el('backend').value, device: el('device').value, transport: el('transport').value,
      destination: el('destinationInput').value, destination_port: Number(port), sample_format: el('sampleFormat').value,
      rate: Number(el('rate').value), channels: Number(el('channels').value), packet_ms: Number(el('packetMs').value), profile: el('profile').value,
      http_lan: el('httpLan').checked, playback_confirmed: el('confirmed').checked, autostart: el('autostart').checked};
    if (config.autostart && !config.playback_confirmed) { notify('Autostart requires a tested, confirmed playback source.', true); return; }
    api('POST', '/v1/config', config, function (error, result) {
      if (error) { notify(error.message, true); return; }
      loadConfig(result); pendingBoot = result.autostart;
      window.AudioCastHomebrew.rootAction(result.autostart ? 'enable' : 'disable', function (hookError) {
        if (hookError) { notify('Configuration saved, but startup hook failed: ' + hookError.message + '. Boot state is NOT verified; retry Apply.', true); }
        else { pendingBoot = null; notify('Configuration saved. Homebrew startup hook ' + (result.autostart ? 'enabled' : 'disabled') + '.'); }
      });
    });
  }
  function loadLogs() { api('GET', '/v1/logs', null, function (error, result) { text('logs', error ? error.message : (result.lines.join('\n') || 'No engine events yet.')); }); }
  function exportDiagnostics() {
    api('GET', '/v1/diagnostics', null, function (error, result) {
      if (error) { notify(error.message, true); return; }
      var output = JSON.stringify(result, null, 2); text('diagnosticOutput', output); el('diagnosticOutput').hidden = false;
      if (window.Blob && window.URL && window.URL.createObjectURL) {
        var blob = new Blob([output], {type: 'application/json'}), url = window.URL.createObjectURL(blob), a = document.createElement('a');
        a.href = url; a.download = 'audiocast-diagnostics.json'; a.click(); window.setTimeout(function () { window.URL.revokeObjectURL(url); }, 3000);
      }
      notify('Sanitized diagnostics displayed. Download depends on TV browser support; SSH diagnose.sh is the reliable fallback.');
    });
  }
  function visible(node) { return !node.disabled && node.getClientRects().length > 0; }
  function focusables() {
    var nodes = (editing ? el('editor') : document).querySelectorAll('button, input, select, a[href]'), result = [], i;
    for (i = 0; i < nodes.length; i++) { if (visible(nodes[i])) { result.push(nodes[i]); } } return result;
  }
  function navigate(direction) {
    var current = document.activeElement, nodes = focusables(), rect = current && current.getBoundingClientRect(), best = null, score = Infinity, i;
    if (!rect || nodes.indexOf(current) < 0) { if (nodes.length) { nodes[0].focus(); } return; }
    for (i = 0; i < nodes.length; i++) {
      if (nodes[i] === current) { continue; } var r = nodes[i].getBoundingClientRect(), dx = (r.left + r.right - rect.left - rect.right) / 2, dy = (r.top + r.bottom - rect.top - rect.bottom) / 2;
      var forward = direction === 37 ? -dx : direction === 39 ? dx : direction === 38 ? -dy : dy;
      var lateral = direction === 37 || direction === 39 ? Math.abs(dy) : Math.abs(dx);
      var s = forward + lateral * 2.5;
      if (forward > 5 && s < score) { score = s; best = nodes[i]; }
    }
    if (best) { best.focus(); best.scrollIntoView({block: 'nearest', inline: 'nearest'}); }
  }
  function editorOpen(input) { editing = input; editFocus = input; editText = input.value; text('editValue', editText || ' '); text('editorTitle', input.parentNode.firstChild.textContent || 'Edit value'); el('editor').hidden = false; el('keyboard').querySelector('button').focus(); }
  function editorClose(saveValue) { if (!editing) { return; } if (saveValue) { editing.value = editText; if (editing.id === 'device') { el('confirmed').checked = false; } } editing = null; el('editor').hidden = true; if (editFocus) { editFocus.focus(); } }
  function init() {
    var pages = document.querySelectorAll('[data-page]'), i;
    for (i = 0; i < pages.length; i++) { pages[i].onclick = function () { showPage(this.getAttribute('data-page')); }; }
    el('reconnect').onclick = bootstrap; el('start').onclick = function () { action('/v1/start'); }; el('stop').onclick = function () { action('/v1/stop'); };
    el('testCapture').onclick = function () { action('/v1/test-capture'); }; el('testReceiver').onclick = function () { action('/v1/test-connection'); };
    el('pause').onclick = function () { action('/v1/suspend'); }; el('resume').onclick = function () { action('/v1/resume'); };
    el('configForm').onsubmit = save; el('refreshDevices').onclick = loadDevices; el('refreshLogs').onclick = loadLogs; el('export').onclick = exportDiagnostics;
    el('deviceList').onchange = function () { if (this.value) { el('device').value = this.value; el('confirmed').checked = false; } };
    var invalidate = ['device', 'backend', 'rate', 'channels', 'sampleFormat'];
    for (i = 0; i < invalidate.length; i++) { el(invalidate[i]).onchange = function () { el('confirmed').checked = false; }; }
    el('showToken').onclick = function () { if (!credentials) { notify('Connect the service first.', true); return; } el('credential').hidden = !el('credential').hidden; text('credential', el('credential').hidden ? '' : 'HTTP Bearer token (private; store in a receiver token file):\n' + credentials.stream_token); };
    el('shellDiagnostic').onclick = function () { window.AudioCastHomebrew.rootAction('diagnose', function (error, output) { el('diagnosticOutput').hidden = false; text('diagnosticOutput', error ? error.message : output); }); };
    var keys = 'abcdefghijklmnopqrstuvwxyz0123456789.:-,_';
    for (i = 0; i < keys.length; i++) { var button = document.createElement('button'); button.textContent = keys.charAt(i); button.setAttribute('data-char', keys.charAt(i)); button.onclick = function () { if (editing && editText.length < (Number(editing.maxLength) || 253)) { editText += this.getAttribute('data-char'); text('editValue', editText); } }; el('keyboard').appendChild(button); }
    el('editBackspace').onclick = function () { editText = editText.slice(0, -1); text('editValue', editText || ' '); }; el('editClear').onclick = function () { editText = ''; text('editValue', ' '); };
    el('editDone').onclick = function () { editorClose(true); }; el('editCancel').onclick = function () { editorClose(false); };
    document.addEventListener('keydown', function (event) {
      var code = event.keyCode, active = document.activeElement;
      if (code === 10009 || code === 27) { event.preventDefault(); if (editing) { editorClose(false); } else if (page !== 'dashboard') { showPage('dashboard'); document.querySelector('[data-page="dashboard"]').focus(); } else if (window.webOS && window.webOS.platformBack) { window.webOS.platformBack(); } else { window.close(); } return; }
      if (code >= 37 && code <= 40) {
        event.preventDefault();
        if (active && active.tagName === 'SELECT' && (code === 37 || code === 39)) { active.selectedIndex = Math.max(0, Math.min(active.options.length - 1, active.selectedIndex + (code === 39 ? 1 : -1))); if (active.onchange) { active.onchange(); } }
        else { navigate(code); } return;
      }
      if (code === 13) { event.preventDefault(); if (active && active.getAttribute('data-edit')) { editorOpen(active); } else if (active && active.tagName !== 'SELECT') { active.click(); } }
    });
    window.addEventListener('unload', function () { if (timer) { window.clearTimeout(timer); } credentials = null; });
    document.querySelector('[data-page="dashboard"]').focus(); bootstrap();
  }
  init();
}());
