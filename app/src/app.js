/* SPDX-License-Identifier: MIT */
(function () {
  'use strict';
  var credentials = null, pollTimer = null, connectGeneration = 0, connected = false;
  var current = null, savedConfig = null, dirty = false, devices = {}, devicesKnown = false, safeCount = 0, mstar = false;
  var saving = false, commanding = false, editing = null, editText = '', editorOrigin = null;
  var bootText = 'Background service startup: checking…';
  function el(id) { return document.getElementById(id); }
  function notice(text, kind) { el('message').textContent = text; el('message').className = 'notice ' + (kind || ''); }
  function toast(text, kind) {
    var node = el('toast'); node.textContent = text; node.className = kind || ''; node.hidden = false;
    clearTimeout(node._timer); node._timer = setTimeout(function () { node.hidden = true; }, 5500);
  }
  function connection(ok, text) {
    connected = ok; el('connection').textContent = text; el('connection').className = 'connection ' + (ok ? 'ok' : 'bad');
    if (!ok) { el('meterL').style.width = '0%'; el('meterR').style.width = '0%'; el('levelL').textContent = '—'; el('levelR').textContent = '—'; }
    updateControls();
  }
  function api(method, path, body, callback) {
    if (!credentials) { callback(new Error('Service not connected')); return; }
    var xhr = new XMLHttpRequest(), finished = false;
    function done(error, result) { if (finished) return; finished = true; callback(error, result); }
    xhr.open(method, 'http://127.0.0.1:7071' + path, true); xhr.timeout = 5000;
    xhr.setRequestHeader('Authorization', 'Bearer ' + credentials.control_token);
    if (body !== undefined) xhr.setRequestHeader('Content-Type', 'application/json');
    xhr.onload = function () {
      var result;
      try { result = JSON.parse(xhr.responseText); } catch (error) { done(new Error('Invalid daemon response')); return; }
      if (xhr.status < 200 || xhr.status >= 300) { done(new Error(result.error || ('HTTP ' + xhr.status))); return; }
      done(null, result);
    };
    xhr.onerror = function () { done(new Error('Background service unreachable')); };
    xhr.ontimeout = function () { done(new Error('Background service timeout')); };
    xhr.send(body === undefined ? null : JSON.stringify(body));
  }
  function selectedSafe() { var device = devices[el('device').value]; return !!device && device.safe_to_open !== false && !device.microphone; }
  function selectedDile() { var device = devices[el('device').value]; return !!device && device.backend === 'dile' && device.id === 'mstar-upload:0'; }
  function sourceWarning() {
    if (selectedDile()) {
      el('sourceWarning').className = 'source-warning blocked';
      el('sourceWarning').textContent = 'Experimental MStar playback tap · 48 kHz stereo / 16-bit only. Capture runs continuously until you stop it; automatic streaming is disabled. No speaker-volume or routing changes.';
    } else if (safeCount) {
      el('sourceWarning').className = 'source-warning';
      el('sourceWarning').textContent = 'Inventory alone does not prove playback capture. Play known audio, test the source, then confirm below. Never use a microphone.';
    }
  }
  function active() { return !!(current && (current.streaming || current.test_active)); }
  function updateControls() {
    var busy = active(), safe = devicesKnown && selectedSafe(), dile = selectedDile(), allowed = safe && (!dile || el('dileGainConfirmed').checked);
    el('dileGainRow').hidden = !dile;
    el('dileGainConfirmed').disabled = !connected || !dile || busy || saving || commanding;
    el('start').disabled = !connected || !allowed || !el('confirmed').checked || busy || saving || commanding;
    el('testCapture').disabled = !connected || !allowed || (dile && !el('confirmed').checked) || busy || saving || commanding;
    el('stop').disabled = !connected || (!busy && !commanding);
    el('applyDest').disabled = !connected || busy || saving;
    el('saveAdvanced').disabled = !connected || busy || saving;
    el('autostart').disabled = !connected || !safe || dile || !el('confirmed').checked || busy || saving;
    el('confirmed').disabled = !connected || !safe || busy;
    el('device').disabled = !connected || safeCount === 0 || busy;
    var fields = ['destIp', 'destPort', 'transport', 'rate', 'channels', 'sampleFormat', 'httpLan'];
    for (var i = 0; i < fields.length; i++) el(fields[i]).disabled = !connected || busy || saving || (dile && (fields[i] === 'rate' || fields[i] === 'channels' || fields[i] === 'sampleFormat'));
    el('start').textContent = dile ? 'Start streaming (continuous)' : 'Start streaming';
  }
  function db(value) { return typeof value === 'number' && value > 0 ? (20 * Math.log(value) / Math.LN10).toFixed(1) + ' dBFS' : '—'; }
  function meter(value) { return value > 0 ? Math.max(0, Math.min(100, ((20 * Math.log(value) / Math.LN10) + 60) / 60 * 100)) : 0; }
  function loadConfig(config, force) {
    savedConfig = config;
    if (bootText.indexOf('Background service starts with the TV.') === 0) bootText = 'Background service starts with the TV. Streaming is ' + (config.autostart ? 'automatic.' : 'manual.');
    if ((dirty || editing) && !force) return;
    el('destIp').value = config.destination || '';
    el('destPort').value = String(config.destination_port || 5004);
    el('transport').value = config.transport || 'rtp';
    el('device').value = config.device || '';
    el('rate').value = String(config.rate || 48000);
    el('channels').value = String(config.channels || 2);
    el('sampleFormat').value = config.sample_format || 'S16_LE';
    el('httpLan').checked = !!config.http_lan;
    el('dileGainConfirmed').checked = !!config.dile_gain_confirmed;
    el('confirmed').checked = !!config.playback_confirmed;
    el('autostart').checked = !!config.autostart;
  }
  function render(status) {
    current = status;
    connection(true, '● Background service online');
    if (status.config) loadConfig(status.config, false);
    var capturing = status.state === 'capturing', busy = status.streaming || status.test_active;
    var blocked = devicesKnown && safeCount === 0;
    el('streamState').textContent = blocked ? 'UNSUPPORTED SOURCE' : (status.test_active ? 'TESTING' : String(status.state || 'idle').toUpperCase());
    el('streamState').className = 'state-pill' + (blocked || status.last_error ? ' error' : capturing ? ' capturing' : '');
    el('signalText').textContent = blocked ? 'Playback capture unavailable' : capturing ? (status.signal === 'detected' ? 'Audio signal detected' : 'Capturing · no signal') : busy ? 'Opening capture source…' : 'Ready when you are';
    var fmt = status.format || {}, levels = status.levels || {}, rms = levels.rms || [], peak = levels.peak || [];
    el('meterL').style.width = (capturing ? meter(rms[0] || 0) : 0) + '%';
    el('meterR').style.width = (capturing ? meter(rms[1] || 0) : 0) + '%';
    el('levelL').textContent = capturing ? db(peak[0]) : '—';
    el('levelR').textContent = fmt.channels === 1 ? 'Mono' : capturing ? db(peak[1]) : '—';
    el('destStatus').textContent = status.transport === 'http' ? 'HTTP · TV port 7070' : String(status.transport || 'rtp').toUpperCase() + ' → ' + (status.destination || 'Not configured');
    el('fmtStatus').textContent = (fmt.rate ? fmt.rate / 1000 + ' kHz' : '—') + ' · ' + (fmt.channels === 1 ? 'Mono' : 'Stereo') + ' · ' + (fmt.sample_format === 'S32_LE' ? '32-bit' : '16-bit PCM');
    el('tvOutput').textContent = status.audio_output && status.audio_output !== 'unknown' ? status.audio_output : 'Not reported by TV';
    el('resources').textContent = (typeof status.cpu_percent === 'number' ? status.cpu_percent.toFixed(1) + '%' : '—') + ' / ' + (typeof status.rss_kb === 'number' ? (status.rss_kb / 1024).toFixed(1) + ' MiB' : '—');
    el('lastError').textContent = status.last_error || '';
    el('bootStatus').textContent = bootText;
    updateControls();
  }
  function schedulePoll(generation, delay) { clearTimeout(pollTimer); pollTimer = setTimeout(function () { poll(generation); }, delay); }
  function poll(generation) {
    if (generation !== connectGeneration) return;
    api('GET', '/v1/status', undefined, function (error, status) {
      if (generation !== connectGeneration) return;
      if (error) { connection(false, '● Service connection lost'); notice(error.message + '. Select Reconnect to retry Homebrew.', 'error'); }
      else { var recovered = !connected; render(status); if (recovered) loadDevices(); }
      schedulePoll(generation, error ? 2000 : 700);
    });
  }
  function loadDevices(callback) {
    api('GET', '/v1/devices', undefined, function (error, result) {
      if (error) { el('sourceWarning').textContent = error.message; if (callback) callback(error); return; }
      var selected = el('device').value || (savedConfig && savedConfig.device) || '', list = result.devices || [];
      var select = el('device'); select.innerHTML = ''; devices = {}; safeCount = 0; mstar = !!result.soc_mstar_mad;
      var placeholder = document.createElement('option'); placeholder.value = ''; placeholder.textContent = 'Select a playback capture device'; select.appendChild(placeholder);
      for (var i = 0; i < list.length; i++) {
        var device = list[i]; device.microphone = /microphone|\bmic\b|voice/i.test(device.name || ''); devices[device.id] = device;
        var option = document.createElement('option'); option.value = device.id;
        option.textContent = device.id + ' · ' + device.name + (device.safe_to_open === false ? ' · UNSAFE / BLOCKED' : device.microphone ? ' · MICROPHONE / BLOCKED' : device.backend === 'dile' ? ' · EXPERIMENTAL / continuous' : device.candidate ? ' · playback candidate' : ' · unverified');
        option.disabled = device.safe_to_open === false || device.microphone;
        if (!option.disabled) safeCount++;
        select.appendChild(option);
      }
      devicesKnown = true;
      if (!safeCount) {
        placeholder.textContent = mstar ? 'MStar TV · no supported playback source' : 'No safe capture source detected';
        el('sourceWarning').textContent = mstar ? 'This MStar TV needs a vendor playback backend. Its MAD ALSA devices hang the kernel, so they are blocked. Media audio capture is not working yet.' : 'No safe playback capture interface is available. Microphone and default inputs are never selected.';
        el('sourceWarning').className = 'source-warning blocked';
        notice('Service connected. Capture is not yet supported on this TV. You can save the receiver destination; streaming is blocked for safety.', 'error');
      } else {
        el('sourceWarning').className = 'source-warning';
        el('sourceWarning').textContent = 'Inventory alone does not prove playback capture. Play known audio, test the source, then confirm below. Never use a microphone.';
        notice(dirty ? 'Unsaved changes · save to apply them.' : 'Service connected. Set your receiver destination and verify a playback capture source.', '');
      }
      select.value = devices[selected] && devices[selected].safe_to_open !== false && !devices[selected].microphone ? selected : '';
      if (!selectedSafe()) { el('confirmed').checked = false; el('autostart').checked = false; }
      sourceWarning();
      updateControls(); if (current) render(current); if (callback) callback(null);
    });
  }
  function changed() { dirty = true; notice('Unsaved changes · save to apply them.', ''); updateControls(); }
  function save(callback) {
    if (saving) return;
    var host = el('destIp').value.replace(/^\s+|\s+$/g, ''), port = el('destPort').value;
    function invalid(text) { toast(text, 'error'); if (callback) callback(new Error(text)); }
    if (!/^[A-Za-z0-9][A-Za-z0-9.:-]{0,252}$/.test(host)) { invalid('Enter a valid receiver IP address or hostname.'); return; }
    if (!/^\d+$/.test(port) || Number(port) < 1024 || Number(port) > 65535) { invalid('UDP port must be between 1024 and 65535.'); return; }
    if (el('autostart').checked && (!selectedSafe() || !el('confirmed').checked)) { invalid('Verify a safe playback source before enabling automatic streaming.'); return; }
    var dile = selectedDile();
    if (dile && el('autostart').checked) { invalid('Automatic streaming is not allowed for the experimental MStar backend.'); return; }
    var config = { destination: host, destination_port: Number(port), transport: el('transport').value,
      backend: dile ? 'dile' : 'alsa', dile_gain_confirmed: dile && el('dileGainConfirmed').checked,
      device: el('device').value, rate: Number(el('rate').value), channels: Number(el('channels').value),
      sample_format: el('sampleFormat').value, http_lan: el('httpLan').checked,
      playback_confirmed: selectedSafe() && el('confirmed').checked, autostart: el('autostart').checked };
    if (config.sample_format !== 'S16_LE' && config.transport !== 'http') { invalid('UDP / RTP requires 16-bit PCM. Choose HTTP for 32-bit audio.'); return; }
    saving = true; updateControls();
    api('POST', '/v1/config', config, function (error, result) {
      saving = false;
      if (error) { updateControls(); toast(error.message, 'error'); if (callback) callback(error); return; }
      dirty = false; loadConfig(result, true); updateControls();
      var text = 'Configuration saved' + (result.autostart ? ' · streaming at TV startup enabled.' : '. Background service still starts with the TV.');
      notice(text + (safeCount === 0 ? ' Playback capture remains unavailable.' : ''), safeCount === 0 ? '' : 'success'); toast('Destination and audio settings saved', 'success');
      if (callback) callback(null, result);
    });
  }
  function command(path, text) {
    if (commanding) return;
    function run(error) {
      if (error) return;
      commanding = true; updateControls();
      api('POST', path, {}, function (error, result) {
        commanding = false; updateControls();
        if (error) { toast(error.message, 'error'); notice(error.message, 'error'); }
        else { toast((result && (result.note || result.message)) || text, 'success'); }
        schedulePoll(connectGeneration, 20);
      });
    }
    if (dirty && path !== '/v1/stop') save(run); else run(null);
  }
  function toggle(id, button) {
    var panel = el(id); panel.hidden = !panel.hidden; el(button).setAttribute('aria-expanded', panel.hidden ? 'false' : 'true');
    if (!panel.hidden) ensureVisible(panel);
  }
  function ensureVisible(node) {
    var r = node.getBoundingClientRect();
    if (r.bottom > innerHeight - 25) window.scrollBy(0, r.bottom - innerHeight + 25);
    else if (r.top < 20) window.scrollBy(0, r.top - 20);
  }
  function focusables() {
    var container = document.querySelector('.kb') || document;
    return Array.prototype.filter.call(container.querySelectorAll('button, input, select'), function (node) { return !node.disabled && node.getClientRects().length > 0; });
  }
  function navigate(code) {
    var nodes = focusables(), currentNode = document.activeElement, rect;
    if (nodes.indexOf(currentNode) < 0) { if (nodes.length) nodes[0].focus(); return; }
    rect = currentNode.getBoundingClientRect(); var best = null, bestScore = Infinity;
    for (var i = 0; i < nodes.length; i++) {
      if (nodes[i] === currentNode) continue;
      var other = nodes[i].getBoundingClientRect(), dx = (other.left + other.right - rect.left - rect.right) / 2, dy = (other.top + other.bottom - rect.top - rect.bottom) / 2;
      var forward = code === 37 ? -dx : code === 39 ? dx : code === 38 ? -dy : dy;
      var lateral = code === 37 || code === 39 ? Math.abs(dy) : Math.abs(dx), score = forward + lateral * 3;
      if (forward > 5 && score < bestScore) { best = nodes[i]; bestScore = score; }
    }
    if (best) { best.focus(); ensureVisible(best); }
  }
  function openEditor(input) {
    if (editing || input.disabled) return;
    editing = input; editorOrigin = input; editText = input.value;
    var overlay = document.createElement('div'); overlay.className = 'kb-overlay';
    overlay.innerHTML = '<div class="kb" role="dialog" aria-modal="true" aria-labelledby="kbTitle"><h3 id="kbTitle"></h3><span id="kbVal" class="kb-value"></span><div class="kb-grid" id="keyboard"></div><div class="kb-actions"><button id="kbDel">Delete</button><button id="kbClear">Clear</button><button id="kbDone" class="primary">Done</button><button id="kbCancel">Cancel</button></div></div>';
    document.body.appendChild(overlay); el('kbTitle').textContent = input.id === 'destPort' ? 'Receiver UDP port' : 'Receiver IP address or hostname';
    function refresh() { el('kbVal').textContent = editText || ' '; }
    var chars = input.id === 'destPort' ? '1234567890' : '1234567890.abcdefghijklmnopqrstuvwxyz-:';
    for (var i = 0; i < chars.length; i++) {
      (function (character) {
        var button = document.createElement('button'); button.textContent = character;
        button.onclick = function () { if (editText.length < (input.id === 'destPort' ? 5 : 253)) { editText += character; refresh(); } };
        el('keyboard').appendChild(button);
      }(chars.charAt(i)));
    }
    el('kbDel').onclick = function () { editText = editText.slice(0, -1); refresh(); };
    el('kbClear').onclick = function () { editText = ''; refresh(); };
    el('kbDone').onclick = function () { closeEditor(true); };
    el('kbCancel').onclick = function () { closeEditor(false); };
    refresh(); el('keyboard').querySelector('button').focus();
  }
  function closeEditor(commit) {
    if (!editing) return;
    var origin = editorOrigin;
    if (commit) { editing.value = editText; changed(); }
    var overlay = document.querySelector('.kb-overlay'); if (overlay) overlay.parentNode.removeChild(overlay);
    editing = null; editorOrigin = null;
    if (origin) origin.focus();
  }
  function connect() {
    var generation = ++connectGeneration;
    clearTimeout(pollTimer); credentials = null; connection(false, 'Connecting to Homebrew…');
    notice('Connecting to the Homebrew background service…', ''); el('reconnect').disabled = true;
    AudioCastHomebrew.rootAction('ensure', function (error, result) {
      if (generation !== connectGeneration) return;
      el('reconnect').disabled = false;
      if (error) { connection(false, '● Homebrew unavailable'); notice(error.message + ' · Select Reconnect to retry.', 'error'); return; }
      credentials = result;
      // Service boot startup is independent of the stream-autostart preference. Never disable the boot service when automatic streaming is off.
      AudioCastHomebrew.rootAction('enable', function (error) {
        if (generation !== connectGeneration) return;
        bootText = error ? 'Boot startup failed: ' + error.message : 'Background service starts with the TV. Streaming is ' + (savedConfig && savedConfig.autostart ? 'automatic.' : 'manual.');
        el('bootStatus').textContent = bootText;
      });
      var attempts = 0;
      function ready() {
        api('GET', '/v1/status', undefined, function (error, status) {
          if (generation !== connectGeneration) return;
          if (error && attempts++ < 6) { setTimeout(ready, 500); return; }
          if (error) { connection(false, '● Daemon unavailable'); notice(error.message + '. Select Reconnect to retry.', 'error'); return; }
          render(status); loadDevices(); schedulePoll(generation, 700);
        });
      }
      ready();
    });
  }
  function init() {
    el('meterL').style.width = '0%'; el('meterR').style.width = '0%';
    el('configForm').onsubmit = function (event) { event.preventDefault(); save(); };
    el('saveAdvanced').onclick = function () { save(); };
    el('start').onclick = function () { command('/v1/start', 'Streaming requested. Verify reception on your receiver.'); };
    el('stop').onclick = function () { command('/v1/stop', 'Audio stream stopped'); };
    el('testCapture').onclick = function () { command('/v1/test-capture', 'Capture test started · no network or recording'); };
    el('refreshDevices').onclick = function () { loadDevices(); };
    el('advancedToggle').onclick = function () { toggle('advanced', 'advancedToggle'); };
    el('diagnosticsToggle').onclick = function () { toggle('diagnosticsPanel', 'diagnosticsToggle'); };
    el('reconnect').onclick = connect;
    el('destIp').onclick = function () { openEditor(this); };
    el('destPort').onclick = function () { openEditor(this); };
    var fields = ['destIp', 'destPort', 'transport', 'rate', 'channels', 'sampleFormat', 'httpLan', 'confirmed', 'dileGainConfirmed', 'autostart', 'device'];
    for (var i = 0; i < fields.length; i++) { el(fields[i]).onchange = changed; el(fields[i]).oninput = changed; }
    el('device').onchange = function () {
      el('confirmed').checked = false; el('autostart').checked = false; el('dileGainConfirmed').checked = false;
      if (selectedDile()) { el('rate').value = '48000'; el('channels').value = '2'; el('sampleFormat').value = 'S16_LE'; }
      sourceWarning(); changed();
    };
    el('confirmed').onchange = function () { if (!this.checked) el('autostart').checked = false; changed(); };
    el('refreshLogs').onclick = function () {
      api('GET', '/v1/logs', undefined, function (error, result) { el('logs').textContent = error ? error.message : ((result.lines || []).join('\n') || 'No engine events.'); });
    };
    el('exportDiag').onclick = function () {
      api('GET', '/v1/diagnostics', undefined, function (error, result) { el('diagOutput').hidden = false; el('diagOutput').textContent = JSON.stringify(error ? { error: error.message } : result, null, 2); });
    };
    document.addEventListener('keydown', function (event) {
      var code = event.keyCode, focused = document.activeElement;
      if (code === 27 || code === 10009 || code === 461) {
        event.preventDefault();
        if (editing) closeEditor(false);
        else if (!el('advanced').hidden) { el('advanced').hidden = true; el('advancedToggle').setAttribute('aria-expanded', 'false'); el('advancedToggle').focus(); }
        else if (!el('diagnosticsPanel').hidden) { el('diagnosticsPanel').hidden = true; el('diagnosticsToggle').setAttribute('aria-expanded', 'false'); el('diagnosticsToggle').focus(); }
        else if (window.webOS && window.webOS.platformBack) window.webOS.platformBack();
        else window.close();
        return;
      }
      if (editing && (code === 8 || code === 46)) { event.preventDefault(); el('kbDel').click(); return; }
      if (editing && event.key && event.key.length === 1 && /^[A-Za-z0-9.:-]$/.test(event.key)) {
        event.preventDefault();
        if (editText.length < (editing.id === 'destPort' ? 5 : 253) && (editing.id !== 'destPort' || /^[0-9]$/.test(event.key))) { editText += event.key; el('kbVal').textContent = editText; }
        return;
      }
      if (code >= 37 && code <= 40) {
        event.preventDefault();
        if (focused && focused.tagName === 'SELECT' && (code === 37 || code === 39)) {
          var next = focused.selectedIndex, step = code === 39 ? 1 : -1;
          do { next += step; } while (next >= 0 && next < focused.options.length && focused.options[next].disabled);
          if (next >= 0 && next < focused.options.length) { focused.selectedIndex = next; if (focused.onchange) focused.onchange(); }
        } else navigate(code);
        return;
      }
      if (code === 13) {
        event.preventDefault();
        if (focused && !focused.disabled) {
          if (focused.tagName === 'SELECT') { /* Left/right changes native select values; up/down navigates. */ }
          else if (focused.tagName === 'INPUT' && focused.type === 'checkbox') { focused.checked = !focused.checked; if (focused.onchange) focused.onchange(); }
          else focused.click();
        }
      }
    });
    updateControls(); connect();
  }
  init();
}());
