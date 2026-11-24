/* Esp32LeafInverterBridge web UI - SPDX-License-Identifier: GPL-3.0-only
 * Plain JavaScript, no dependencies. Talks to the REST API and the /ws live feed
 * documented in main/web_server.h. */
'use strict';

(() => {
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  // ---------------------------------------------------------------- storage
  // Browser storage can be unavailable (private mode, captive-portal views).
  const store = {
    get(key, session) {
      try { return (session ? sessionStorage : localStorage).getItem(key); } catch (e) { return null; }
    },
    set(key, value, session) {
      try {
        const s = session ? sessionStorage : localStorage;
        if (value === null || value === undefined) s.removeItem(key); else s.setItem(key, value);
      } catch (e) { /* ignore */ }
    },
  };

  // ---------------------------------------------------------------- state
  const state = {
    token: store.get('leafbridge.token', true),
    lastPassword: null,          // kept briefly after login for the forced password change
    lastPasswordTimer: null,
    status: null,
    lastStatusAt: 0,
    settings: null,
    original: {},
    draft: {},
    session: { authenticated: false, default_password: false, default_wifi_password: false },
    system: null,
    view: 'dashboard',
    file: null,
    uploading: false,
  };

  const SETTING_TITLES = {
    Vehicle: 'Vehicle',
    Inverter: 'Inverter upgrade',
    Battery: 'Battery pack',
    BatterySaver: 'Battery saver (charge limit)',
    Glide: 'Glide in drive',
    Capacity: 'Capacity boost',
    CurrentControl: 'Charging power limit',
  };
  const INVERTER_DESC = {
    110: 'Inverter of the 2018+ LEAF 40 kWh (ZE1) driving the EM57 motor.',
    160: 'Inverter of the LEAF e+ 62 kWh driving the EM57 motor: the most power.',
    0: 'Forward every frame unchanged, no torque scaling.',
  };
  const GEARS = { P: 'Park', R: 'Reverse', N: 'Neutral', D: 'Drive' };
  const CHARGE = { 0x20: 'AC charging', 0x40: 'Quick charge starting', 0x60: 'Charger idle', 0xC0: 'Quick charging', 0xE0: 'Quick charge ending' };
  const TORQUE_SCALE_NM = 400;

  // ---------------------------------------------------------------- helpers
  class ApiError extends Error {
    constructor(message, status, code, data) {
      super(message);
      this.status = status;
      this.code = code;
      this.data = data;
    }
  }
  class Cancelled extends Error {}

  const fmt = (v, digits = 0) => (v === null || v === undefined || Number.isNaN(v)) ? '–' : Number(v).toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
  const fmtSigned = (v, digits = 0) => (v === null || v === undefined) ? '–' : (v > 0 ? '+' : '') + fmt(v, digits);
  function fmtUptime(s) {
    if (s === undefined || s === null) return '–';
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d) return `${d} d ${h} h`;
    if (h) return `${h} h ${m} min`;
    if (m) return `${m} min ${s % 60} s`;
    return `${s} s`;
  }
  function fmtBytes(n) {
    if (n === undefined || n === null) return '–';
    if (n >= 1048576) return `${fmt(n / 1048576, 1)} MB`;
    if (n >= 1024) return `${fmt(n / 1024, 1)} KB`;
    return `${n} B`;
  }
  function setText(el, text) {
    if (el && el.textContent !== String(text)) el.textContent = text;
  }
  function el(tag, attrs = {}, children = []) {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') node.className = v;
      else if (k === 'text') node.textContent = v;
      else if (v !== false && v !== null && v !== undefined) node.setAttribute(k, v === true ? '' : v);
    }
    for (const c of [].concat(children)) if (c) node.append(c);
    return node;
  }
  function icon(id) {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', `#${id}`);
    svg.append(use);
    return svg;
  }
  function setBusy(btn, busy) {
    if (!btn) return;
    btn.classList.toggle('busy', busy);
    btn.disabled = busy;
  }
  function download(name, blob) {
    const url = URL.createObjectURL(blob);
    const a = el('a', { href: url, download: name });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
  }
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // ---------------------------------------------------------------- toasts
  function toast(message, kind = 'ok', ms = 4500) {
    const t = el('div', { class: `toast ${kind}`, role: kind === 'bad' ? 'alert' : 'status' },
      [icon(kind === 'bad' ? 'i-warn' : 'i-check'), el('span', { text: message })]);
    $('#toasts').append(t);
    setTimeout(() => t.remove(), ms);
  }
  function reportError(e) {
    if (e instanceof Cancelled) return;
    toast(e.message || String(e), 'bad', 6500);
  }

  // ---------------------------------------------------------------- API
  async function api(path, { method = 'GET', body, headers = {}, raw = false } = {}) {
    const h = Object.assign({}, headers);
    if (state.token) h.Authorization = `Bearer ${state.token}`;
    let payload = body;
    if (body !== undefined && !raw) {
      h['Content-Type'] = 'application/json';
      payload = JSON.stringify(body);
    }
    let res;
    try {
      res = await fetch(path, { method, headers: h, body: payload, cache: 'no-store' });
    } catch (e) {
      throw new ApiError('The bridge is not reachable. Check the Wi-Fi connection.', 0, 'network');
    }
    if (raw && res.ok) return res;
    let data = null;
    try { data = await res.json(); } catch (e) { /* not JSON */ }
    if (!res.ok) {
      if (res.status === 401) setToken(null);
      throw new ApiError((data && data.message) || `Request failed (${res.status})`, res.status, data && data.error, data);
    }
    return data;
  }

  function setToken(token) {
    state.token = token;
    store.set('leafbridge.token', token, true);
    state.session.authenticated = !!token;
    renderAccount();
  }

  function rememberPassword(pw) {
    state.lastPassword = pw;
    clearTimeout(state.lastPasswordTimer);
    state.lastPasswordTimer = setTimeout(() => { state.lastPassword = null; }, 5 * 60 * 1000);
  }

  // Runs fn(); logs in or asks for a new password when the bridge requires it.
  async function withAuth(fn) {
    for (let attempt = 0; attempt < 3; attempt++) {
      try {
        return await fn();
      } catch (e) {
        if (e.status === 401 && e.code === 'unauthorized') {
          if (!(await loginDialog())) throw new Cancelled();
          continue;
        }
        if (e.status === 403 && e.code === 'password_change_required') {
          if (!(await setPasswordDialog())) throw new Cancelled();
          continue;
        }
        throw e;
      }
    }
    throw new Cancelled();
  }

  async function ensureReady() {
    if (!state.session.authenticated && !(await loginDialog())) throw new Cancelled();
    if (state.session.default_password && !(await setPasswordDialog())) throw new Cancelled();
  }

  // ---------------------------------------------------------------- dialogs
  function openDialog(dlg) {
    if (typeof dlg.showModal === 'function') dlg.showModal(); else dlg.setAttribute('open', '');
  }
  function closeDialog(dlg) {
    if (typeof dlg.close === 'function') dlg.close(); else dlg.removeAttribute('open');
  }
  $$('dialog').forEach((dlg) => {
    $$('[data-close]', dlg).forEach((b) => b.addEventListener('click', () => closeDialog(dlg)));
  });
  $$('[data-reveal]').forEach((b) => b.addEventListener('click', () => {
    const input = document.getElementById(b.dataset.reveal);
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    b.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
  }));

  let loginPending = null;
  function loginDialog() {
    if (loginPending) return loginPending;
    const dlg = $('#dlg-login');
    const form = $('#form-login');
    const err = $('#login-error');
    const btn = $('#btn-login');
    form.reset();
    setText(err, '');
    loginPending = new Promise((resolve) => {
      let done = false;
      const finish = (ok) => {
        if (done) return;
        done = true;
        form.removeEventListener('submit', onSubmit);
        dlg.removeEventListener('close', onClose);
        loginPending = null;
        resolve(ok);
      };
      const onClose = () => finish(state.session.authenticated);
      const onSubmit = async (ev) => {
        ev.preventDefault();
        const password = form.password.value;
        setText(err, '');
        setBusy(btn, true);
        try {
          const r = await api('/api/login', { method: 'POST', body: { password } });
          setToken(r.token);
          state.session.default_password = r.default_password;
          if (r.default_password) rememberPassword(password);
          renderSecurity();
          closeDialog(dlg);
          toast('Logged in');
          finish(true);
        } catch (e) {
          setText(err, e.message);
          form.password.select();
        } finally {
          setBusy(btn, false);
        }
      };
      form.addEventListener('submit', onSubmit);
      dlg.addEventListener('close', onClose);
    });
    openDialog(dlg);
    setTimeout(() => $('#login-password').focus(), 50);
    return loginPending;
  }

  let setpwPending = null;
  function setPasswordDialog() {
    if (setpwPending) return setpwPending;
    const dlg = $('#dlg-setpw');
    const form = $('#form-setpw');
    const err = $('#setpw-error');
    form.reset();
    setText(err, '');
    $('#setpw-current').hidden = !!state.lastPassword;
    form.current.required = !state.lastPassword;
    setpwPending = new Promise((resolve) => {
      let done = false;
      const finish = (ok) => {
        if (done) return;
        done = true;
        form.removeEventListener('submit', onSubmit);
        dlg.removeEventListener('close', onClose);
        setpwPending = null;
        resolve(ok);
      };
      const onClose = () => finish(!state.session.default_password);
      const onSubmit = async (ev) => {
        ev.preventDefault();
        const submit = $('button[type="submit"]', form);
        setText(err, '');
        if (form.new.value !== form.repeat.value) { setText(err, 'The two passwords do not match.'); return; }
        setBusy(submit, true);
        try {
          await changePassword(state.lastPassword || form.current.value, form.new.value);
          closeDialog(dlg);
          finish(true);
        } catch (e) {
          if (e.status === 401 && e.code === 'unauthorized') { closeDialog(dlg); finish(false); loginDialog(); return; }
          setText(err, e.message);
          if (e.code === 'bad_password') { $('#setpw-current').hidden = false; form.current.required = true; state.lastPassword = null; }
        } finally {
          setBusy(submit, false);
        }
      };
      form.addEventListener('submit', onSubmit);
      dlg.addEventListener('close', onClose);
    });
    openDialog(dlg);
    setTimeout(() => (state.lastPassword ? form.new : form.current).focus(), 50);
    return setpwPending;
  }

  function confirmDialog(title, text, okLabel = 'Confirm', danger = true) {
    const dlg = $('#dlg-confirm');
    setText($('#dlg-confirm-title'), title);
    setText($('#dlg-confirm-text'), text);
    const ok = $('#dlg-confirm-ok');
    setText(ok, okLabel);
    ok.className = `btn ${danger ? 'btn-danger' : 'btn-primary'}`;
    dlg.returnValue = '';
    return new Promise((resolve) => {
      dlg.addEventListener('close', () => resolve(dlg.returnValue === 'ok'), { once: true });
      openDialog(dlg);
    });
  }

  async function changePassword(current, next) {
    const r = await api('/api/password', { method: 'POST', body: { current, new: next } });
    if (r.token) setToken(r.token); else setToken(null);
    state.session.default_password = false;
    state.lastPassword = null;
    renderSecurity();
    toast('Admin password changed. Other sessions were signed out.');
  }

  // ---------------------------------------------------------------- connection
  let ws = null;
  let wsFailures = 0;
  let pollTimer = null;

  function setConn(kind, text) {
    const pill = $('#conn');
    pill.className = `pill pill-${kind}`;
    setText($('#conn-text'), text);
  }

  function connectWs() {
    if (!('WebSocket' in window)) { startPolling(); return; }
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    try {
      ws = new WebSocket(`${proto}://${location.host}/ws`);
    } catch (e) {
      startPolling();
      return;
    }
    ws.onopen = () => { wsFailures = 0; stopPolling(); };
    ws.onmessage = (ev) => {
      let msg;
      try { msg = JSON.parse(ev.data); } catch (e) { return; }
      if (msg.type === 'status') onStatus(msg.data);
      else if (msg.type === 'settings') loadSettings({ keepDraft: true });
    };
    ws.onclose = () => {
      ws = null;
      wsFailures++;
      if (wsFailures >= 2) startPolling();
      setTimeout(connectWs, Math.min(1000 * wsFailures, 5000));
    };
    ws.onerror = () => { try { ws.close(); } catch (e) { /* ignore */ } };
  }

  function startPolling() {
    if (pollTimer) return;
    const poll = async () => {
      try { onStatus(await api('/api/status')); } catch (e) { /* watchdog shows offline */ }
    };
    poll();
    pollTimer = setInterval(poll, 1000);
  }
  function stopPolling() {
    clearInterval(pollTimer);
    pollTimer = null;
  }

  setInterval(() => {
    const age = Date.now() - state.lastStatusAt;
    if (!state.lastStatusAt) return;
    if (age > 10000) setConn('bad', 'Offline');
    else if (age > 3000) setConn('wait', 'Reconnecting');
    else setConn('ok', pollTimer ? 'Connected' : 'Live');
  }, 1000);

  // ---------------------------------------------------------------- dashboard
  function setBar(barId, valId, value, digits) {
    const bar = document.getElementById(barId);
    const width = Math.min(Math.abs(value || 0) / TORQUE_SCALE_NM, 1) * 50;
    bar.style.width = `${width}%`;
    bar.style.left = value < 0 ? `${50 - width}%` : '50%';
    bar.classList.toggle('neg', value < 0);
    setText(document.getElementById(valId), `${fmtSigned(value, digits)} Nm`);
  }

  function multiplierChips(container, drive, regen, modify) {
    container.replaceChildren();
    if (!modify) {
      container.append(el('span', { class: 'chip', text: 'No torque changes' }));
      return;
    }
    container.append(el('span', { class: 'chip drive', text: `Drive ×${fmt(drive, 2)}` }));
    container.append(el('span', { class: 'chip regen', text: `Regen ×${fmt(regen, 2)}` }));
  }

  function renderBus(id, b) {
    const root = document.getElementById(id);
    root.className = `bus s-${b.state}`;
    const names = { active: 'OK', warning: 'Warning', passive: 'Error passive', bus_off: 'Bus-off', stopped: 'Not running' };
    setText($('.bus-state', root), names[b.state] || b.state);
    const set = (k, v) => setText($(`[data-k="${k}"]`, root), v);
    set('rx_per_s', `${fmt(b.rx_per_s)} /s`);
    set('tx_per_s', `${fmt(b.tx_per_s)} /s`);
    set('errors', `${b.tec} / ${b.rec}`);
    set('dropped', fmt(b.tx_dropped + b.rx_overflow));
    set('busoff', `${fmt(b.bus_off)} / ${fmt(b.restarts)}`);
  }

  let modeKey = '';
  function onStatus(s) {
    if (!s) return;
    state.status = s;
    state.lastStatusAt = Date.now();

    setText($('#hdr-sub'), `${s.inverter.label} · v${s.firmware}`);
    const gear = $('#gear');
    setText(gear, s.shifter);
    gear.className = `gear g-${s.shifter}`;
    setText($('#gear-name'), GEARS[s.shifter] || s.shifter);
    setText($('#park-note'), s.parked ? 'Settings and updates allowed' : 'Settings and updates locked while driving');

    setText($('#mode-label'), s.inverter.label);
    const key = `${s.inverter.mode}|${s.inverter.drive_multiplier}|${s.inverter.regen_multiplier}|${s.inverter.modify_torque}`;
    if (key !== modeKey) {
      modeKey = key;
      multiplierChips($('#mode-chips'), s.inverter.drive_multiplier, s.inverter.regen_multiplier, s.inverter.modify_torque);
    }

    const soc = s.battery.soc_percent;
    setText($('#soc'), soc === null ? '–' : `${fmt(soc, 1)} %`);
    const circumference = 326.7;
    $('#soc-ring').style.strokeDashoffset = soc === null ? circumference : circumference * (1 - Math.max(0, Math.min(100, soc)) / 100);
    const charge = s.battery.ac_charging ? 'AC charging' : (CHARGE[s.battery.charge_status] || 'Not charging');
    setText($('#charge'), charge);
    setText($('#charge-note'), s.battery.ac_charging ? 'ZE1 message emulation paused while charging'
      : (soc === null ? 'No state of charge received yet' : 'State of charge'));

    setBar('bar-vcm', 'val-vcm', s.torque.vcm_request_nm, 1);
    setBar('bar-out', 'val-out', s.torque.out_request_nm, 1);
    setBar('bar-resp', 'val-resp', s.torque.response_nm, 1);
    setText($('#torque-note'), s.inverter.modify_torque
      ? `In D the bridge multiplies drive torque by ${fmt(s.inverter.drive_multiplier, 2)} and regen by ${fmt(s.inverter.regen_multiplier, 2)}.`
      : 'Torque is passed through unchanged.');

    renderBus('bus-vcm', s.buses.vcm);
    renderBus('bus-inverter', s.buses.inverter);

    const br = s.bridge;
    setText($('#uptime'), `Up ${fmtUptime(s.uptime_s)}`);
    setText($('#lat-avg'), `${fmt(br.latency_us_avg)} µs`);
    setText($('#lat-max'), `${fmt(br.latency_us_max)} µs`);
    setText($('#st-mod'), fmt(br.frames_modified));
    setText($('#st-gen'), fmt(br.frames_generated));
    setText($('#st-clamp'), fmt(br.torque_clamped));
    setText($('#st-queue'), `${fmt(br.queue_high_water)} / ${fmt(br.queue_depth)}`);
    setText($('#lat-peak'), `Worst case since start: ${fmt(br.latency_us_peak)} µs.${br.queue_overflow ? ` ${fmt(br.queue_overflow)} frames lost to a full queue.` : ''}`);

    if (s.security) {
      const changed = state.session.default_password !== s.security.default_password ||
        state.session.default_wifi_password !== s.security.default_wifi_password;
      state.session.default_password = s.security.default_password;
      state.session.default_wifi_password = s.security.default_wifi_password;
      if (changed) renderSecurity();
    }
    renderParked();
  }

  // ---------------------------------------------------------------- settings
  function isDirty() {
    return Object.keys(state.draft).some((k) => state.draft[k] !== state.original[k]);
  }

  function renderSavebar() {
    const dirty = isDirty();
    $('#savebar').hidden = !dirty;
    const parked = !state.status || state.status.parked;
    $('#btn-save').disabled = !parked;
    setText($('#savebar-text'), parked ? 'Unsaved changes' : 'Shift to P to save');
  }

  function renderParked() {
    const parked = !state.status || state.status.parked;
    $('#settings-park').hidden = parked;
    renderSavebar();
    renderInstallButton();
  }

  function renderSettings() {
    const data = state.settings;
    if (!data) return;
    const inv = data.settings.find((s) => s.id === 'Inverter');
    const opts = $('#inverter-options');
    opts.replaceChildren();
    if (inv) {
      inv.options.forEach((o) => {
        const input = el('input', { type: 'radio', name: 'Inverter', value: o.value });
        input.checked = state.draft.Inverter === o.value;
        const chips = el('div', { class: 'chips' });
        multiplierChips(chips, Number(o.drive_multiplier), Number(o.regen_multiplier), o.value !== '0');
        opts.append(el('label', { class: 'option' }, [
          input,
          el('strong', { text: o.label }),
          el('div', { class: 'desc', text: INVERTER_DESC[o.value] || '' }),
          chips,
        ]));
      });
    }
    const others = $('#other-settings');
    others.replaceChildren();
    data.settings.filter((s) => s.id !== 'Inverter').forEach((s) => {
      const select = el('select', { name: s.id });
      s.options.forEach((o) => {
        const opt = el('option', { value: o.value, text: o.label });
        opt.selected = state.draft[s.id] === o.value;
        select.append(opt);
      });
      others.append(el('label', { class: 'field' }, [el('span', { text: SETTING_TITLES[s.id] || s.id }), select]));
    });
    renderSavebar();
  }

  async function loadSettings({ keepDraft = false } = {}) {
    try {
      const data = await api('/api/settings');
      const dirty = keepDraft && isDirty();
      state.settings = data;
      state.original = Object.fromEntries(data.settings.map((s) => [s.id, s.value]));
      if (!dirty) state.draft = Object.assign({}, state.original);
      renderSettings();
    } catch (e) {
      reportError(e);
    }
  }

  $('#settings-form').addEventListener('change', (ev) => {
    const t = ev.target;
    if (t.name) state.draft[t.name] = t.value;
    renderSavebar();
  });
  $('#btn-discard').addEventListener('click', () => {
    state.draft = Object.assign({}, state.original);
    renderSettings();
  });
  $('#settings-form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const btn = $('#btn-save');
    setBusy(btn, true);
    try {
      const r = await withAuth(() => api('/api/settings', { method: 'POST', body: state.draft }));
      toast(r.message || 'Settings saved');
      await loadSettings();
    } catch (e) {
      reportError(e);
    } finally {
      setBusy(btn, false);
      renderSavebar();
    }
  });

  // ---------------------------------------------------------------- firmware
  function renderFirmware() {
    const sys = state.system;
    if (!sys) return;
    setText($('#fw-version'), sys.firmware.version);
    setText($('#fw-built'), sys.firmware.built);
    setText($('#fw-idf'), sys.firmware.idf);
    setText($('#fw-slot'), sys.ota.running || '–');
    setText($('#fw-other'), sys.ota.next_version ? `${sys.ota.next} (v${sys.ota.next_version})` : `${sys.ota.next || '–'} (empty)`);
    setText($('#footer-version'), `firmware ${sys.firmware.version}`);
  }

  function renderInstallButton() {
    const parked = !state.status || state.status.parked;
    $('#btn-install').disabled = !state.file || state.uploading || !parked;
    if (state.file && !parked && !state.uploading) setText($('#fw-status'), 'Shift to P (park) to install the update.');
  }

  function pickFile(file) {
    const status = $('#fw-status');
    state.file = null;
    $('#fw-progress').hidden = true;
    if (!file) { renderInstallButton(); return; }
    const max = state.system ? state.system.ota.slot_bytes : 0;
    if (!/\.bin$/i.test(file.name)) {
      setText(status, 'Choose the application image (a .bin file).');
    } else if (file.size < 1024) {
      setText(status, 'This file is too small to be a firmware image.');
    } else if (max && file.size > max) {
      setText(status, `This file (${fmtBytes(file.size)}) is larger than the update slot (${fmtBytes(max)}).`);
    } else {
      state.file = file;
      setText(status, `Ready to install ${file.name} (${fmtBytes(file.size)}).`);
    }
    $('#drop-text').replaceChildren(
      el('strong', { text: file.name }), el('br'), el('span', { class: 'muted small', text: `${fmtBytes(file.size)} · choose another file` }));
    renderInstallButton();
  }

  const dz = $('#dropzone');
  $('#fw-file').addEventListener('change', (ev) => pickFile(ev.target.files[0]));
  ['dragenter', 'dragover'].forEach((t) => dz.addEventListener(t, (ev) => { ev.preventDefault(); dz.classList.add('drag'); }));
  ['dragleave', 'drop'].forEach((t) => dz.addEventListener(t, () => dz.classList.remove('drag')));
  dz.addEventListener('drop', (ev) => { ev.preventDefault(); pickFile(ev.dataTransfer.files[0]); });

  function uploadFirmware(file, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/update');
      if (state.token) xhr.setRequestHeader('Authorization', `Bearer ${state.token}`);
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
      xhr.onload = () => {
        let data = null;
        try { data = JSON.parse(xhr.responseText); } catch (e) { /* ignore */ }
        if (xhr.status >= 200 && xhr.status < 300) resolve(data || {});
        else {
          if (xhr.status === 401) setToken(null);
          reject(new ApiError((data && data.message) || `Upload failed (${xhr.status})`, xhr.status, data && data.error, data));
        }
      };
      xhr.onerror = () => reject(new ApiError('The connection was lost during the upload. The current firmware stays installed.', 0, 'network'));
      xhr.send(file);
    });
  }

  // Waits until the bridge answers again after a restart; returns /api/system or null.
  async function waitForRestart(timeoutMs = 90000) {
    const start = Date.now();
    await sleep(3000);
    while (Date.now() - start < timeoutMs) {
      try {
        const sys = await api('/api/system');
        if (sys && sys.device.uptime_s < (Date.now() - start) / 1000 + 5) return sys;
      } catch (e) { /* still restarting */ }
      await sleep(2000);
    }
    return null;
  }

  $('#btn-install').addEventListener('click', async () => {
    const file = state.file;
    if (!file) return;
    const btn = $('#btn-install');
    const status = $('#fw-status');
    const bar = $('#fw-progress-bar');
    const progress = $('#fw-progress');
    try {
      await ensureReady();
      state.uploading = true;
      setBusy(btn, true);
      progress.hidden = false;
      progress.className = 'progress';
      bar.style.width = '0%';
      const result = await withAuth(() => uploadFirmware(file, (p) => {
        bar.style.width = `${Math.round(p * 100)}%`;
        setText(status, p < 1 ? `Uploading… ${Math.round(p * 100)} %` : 'Verifying the image…');
      }));
      bar.style.width = '100%';
      setText(status, `Version ${result.version} installed. The bridge is restarting…`);
      const sys = await waitForRestart();
      if (!sys) {
        progress.classList.add('failed');
        setText(status, 'The bridge did not come back yet. Reconnect to its Wi-Fi and reload this page.');
        return;
      }
      state.system = sys;
      renderFirmware();
      if (sys.firmware.version === result.version) {
        progress.classList.add('done');
        setText(status, `Firmware ${sys.firmware.version} is running.`);
        toast(`Firmware ${sys.firmware.version} installed`);
      } else {
        progress.classList.add('failed');
        setText(status, `The new firmware did not pass its self test; the bridge returned to ${sys.firmware.version}.`);
        toast('Update rolled back', 'bad');
      }
      state.file = null;
      $('#fw-file').value = '';
    } catch (e) {
      progress.classList.add('failed');
      if (!(e instanceof Cancelled)) setText(status, e.message);
      reportError(e);
    } finally {
      state.uploading = false;
      setBusy(btn, false);
      renderInstallButton();
    }
  });

  // ---------------------------------------------------------------- device
  function renderSecurity() {
    const s = state.session;
    $('#banner').hidden = !s.default_password;
    const pw = $('#pw-badge');
    pw.className = `badge ${s.default_password ? 'badge-warn' : 'badge-ok'}`;
    setText(pw, s.default_password ? 'Factory default' : 'Custom');
    const wf = $('#wifi-badge');
    wf.className = `badge ${s.default_wifi_password ? 'badge-warn' : 'badge-ok'}`;
    setText(wf, s.default_wifi_password ? 'Default password' : 'Custom');
    renderAccount();
  }

  function renderAccount() {
    const btn = $('#btn-account');
    const signedIn = !!state.token;
    btn.classList.toggle('signed-in', signedIn);
    btn.replaceChildren(icon(signedIn ? 'i-user' : 'i-lock'));
    btn.setAttribute('aria-label', signedIn ? 'Logged in - log out' : 'Log in');
    btn.title = signedIn ? 'Logged in - click to log out' : 'Log in';
  }

  function renderSystem() {
    const sys = state.system;
    if (!sys) return;
    renderFirmware();
    const rows = [
      ['Chip', `${sys.device.model.toUpperCase()} ${sys.device.revision}, ${sys.device.cores} cores @ ${sys.device.cpu_mhz} MHz`],
      ['Flash', fmtBytes(sys.device.flash_bytes)],
      ['MAC', sys.device.mac],
      ['Uptime', fmtUptime(sys.device.uptime_s)],
      ['Last reset', sys.device.reset_reason],
      ['Free memory', `${fmtBytes(sys.memory.heap_free)} (lowest ${fmtBytes(sys.memory.heap_min_free)})`],
      ['Address', sys.network.ip || '–'],
      ['Wi-Fi clients', String(sys.network.clients)],
      ['Firmware', `${sys.firmware.version} (${sys.firmware.elf_sha256})`],
    ];
    sys.tasks.forEach((t) => rows.push([`Stack ${t.name}`, `${fmtBytes(t.stack_free)} free`]));
    const dl = $('#sys-info');
    dl.replaceChildren();
    rows.forEach(([k, v]) => dl.append(el('dt', { text: k }), el('dd', { text: v })));
    $('#dump-box').hidden = !sys.coredump.present;
    $('#dump-actions').hidden = !sys.coredump.present;
    setText($('#dump-size'), fmtBytes(sys.coredump.bytes));
  }

  async function loadSystem() {
    try {
      state.system = await api('/api/system');
      renderSystem();
    } catch (e) {
      reportError(e);
    }
  }

  async function loadNetwork() {
    try {
      const n = await api('/api/network');
      const form = $('#form-wifi');
      if (document.activeElement !== form.ssid) form.ssid.value = n.ssid;
      state.session.default_wifi_password = n.default_password;
      renderSecurity();
    } catch (e) {
      reportError(e);
    }
  }

  $('#form-password').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const form = ev.target;
    const btn = $('button[type="submit"]', form);
    if (form.new.value !== form.repeat.value) { toast('The two new passwords do not match.', 'bad'); return; }
    setBusy(btn, true);
    try {
      if (!state.session.authenticated && !(await loginDialog())) throw new Cancelled();
      await withAuth(() => changePassword(form.current.value, form.new.value));
      form.reset();
    } catch (e) {
      reportError(e);
    } finally {
      setBusy(btn, false);
    }
  });

  $('#form-wifi').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const form = ev.target;
    const btn = $('button[type="submit"]', form);
    if (form.password.value !== form.repeat.value) { toast('The two Wi-Fi passwords do not match.', 'bad'); return; }
    const ok = await confirmDialog('Change the Wi-Fi network?',
      `The access point restarts as "${form.ssid.value}". This phone or computer is disconnected and must rejoin with the new password.`,
      'Change Wi-Fi', false);
    if (!ok) return;
    setBusy(btn, true);
    try {
      const r = await withAuth(() => api('/api/network', { method: 'POST', body: { ssid: form.ssid.value, password: form.password.value } }));
      toast(r.message, 'ok', 9000);
      form.password.value = '';
      form.repeat.value = '';
    } catch (e) {
      reportError(e);
    } finally {
      setBusy(btn, false);
    }
  });

  $('#btn-sys-refresh').addEventListener('click', loadSystem);

  $('#btn-diag').addEventListener('click', async () => {
    try {
      const [status, system, settings] = await Promise.all([api('/api/status'), api('/api/system'), api('/api/settings')]);
      const blob = new Blob([JSON.stringify({ generated: new Date().toISOString(), status, system, settings }, null, 2)], { type: 'application/json' });
      download(`leaf-bridge-diagnostics-${Date.now()}.json`, blob);
    } catch (e) {
      reportError(e);
    }
  });

  $('#btn-dump').addEventListener('click', async () => {
    try {
      const res = await withAuth(() => api('/api/coredump', { raw: true }));
      download('leaf-bridge-coredump.bin', await res.blob());
    } catch (e) {
      reportError(e);
    }
  });
  $('#btn-dump-erase').addEventListener('click', async () => {
    if (!(await confirmDialog('Erase the crash dump?', 'Download it first if you want to report the crash.', 'Erase'))) return;
    try {
      const r = await withAuth(() => api('/api/coredump', { method: 'DELETE' }));
      toast(r.message);
      loadSystem();
    } catch (e) {
      reportError(e);
    }
  });

  async function restartFlow(path, title, text, okLabel, doneText) {
    if (!(await confirmDialog(title, text, okLabel))) return;
    try {
      const r = await withAuth(() => api(path, { method: 'POST' }));
      toast(r.message || doneText, 'ok', 8000);
      setConn('wait', 'Restarting');
      const sys = await waitForRestart();
      if (sys) {
        state.system = sys;
        renderSystem();
        toast('The bridge is back online');
      }
    } catch (e) {
      reportError(e);
    }
  }

  $('#btn-reboot').addEventListener('click', () => restartFlow('/api/reboot', 'Restart the bridge?',
    'CAN forwarding stops for about two seconds. Only do this with the car in P.', 'Restart', 'Restarting'));
  $('#btn-factory').addEventListener('click', () => restartFlow('/api/factory-reset', 'Factory reset?',
    'All settings, the admin password and the Wi-Fi settings return to the factory defaults. You will have to reconnect to the default Wi-Fi network.',
    'Reset everything', 'Factory reset done').then(() => setToken(null)));

  $('#btn-account').addEventListener('click', async () => {
    if (!state.token) { loginDialog(); return; }
    if (!(await confirmDialog('Log out?', 'You will need the admin password for the next change.', 'Log out', false))) return;
    try { await api('/api/logout', { method: 'POST' }); } catch (e) { /* ignore */ }
    setToken(null);
    toast('Logged out');
  });

  $('#banner-action').addEventListener('click', async () => {
    try {
      await ensureReady();
    } catch (e) {
      reportError(e);
    }
  });

  // ---------------------------------------------------------------- theme
  const THEMES = ['auto', 'light', 'dark'];
  function applyTheme(theme) {
    if (theme === 'auto') delete document.documentElement.dataset.theme;
    else document.documentElement.dataset.theme = theme;
    const btn = $('#btn-theme');
    btn.replaceChildren(icon(theme === 'dark' ? 'i-moon' : theme === 'light' ? 'i-sun' : 'i-auto'));
    btn.setAttribute('aria-label', `Colour theme: ${theme === 'auto' ? 'automatic' : theme}`);
  }
  let theme = store.get('leafbridge.theme') || 'auto';
  if (!THEMES.includes(theme)) theme = 'auto';
  applyTheme(theme);
  $('#btn-theme').addEventListener('click', () => {
    theme = THEMES[(THEMES.indexOf(theme) + 1) % THEMES.length];
    store.set('leafbridge.theme', theme);
    applyTheme(theme);
  });

  // ---------------------------------------------------------------- router
  const VIEWS = ['dashboard', 'settings', 'firmware', 'device'];
  function route() {
    const name = (location.hash || '#dashboard').slice(1);
    const view = VIEWS.includes(name) ? name : 'dashboard';
    state.view = view;
    VIEWS.forEach((v) => { document.getElementById(`view-${v}`).hidden = v !== view; });
    $$('.tabs a').forEach((a) => {
      if (a.dataset.view === view) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
    });
    if (view === 'settings') loadSettings({ keepDraft: true });
    if (view === 'firmware' || view === 'device') loadSystem();
    if (view === 'device') loadNetwork();
    window.scrollTo(0, 0);
  }
  window.addEventListener('hashchange', route);

  // ---------------------------------------------------------------- start
  async function init() {
    renderAccount();
    try {
      const s = await api('/api/session');
      state.session.authenticated = s.authenticated;
      state.session.default_password = s.default_password;
      state.session.default_wifi_password = s.default_wifi_password;
      if (!s.authenticated && state.token) setToken(null);
    } catch (e) { /* offline: the connection pill shows it */ }
    renderSecurity();
    route();
    connectWs();
    try { onStatus(await api('/api/status')); } catch (e) { setConn('bad', 'Offline'); }
    api('/api/system').then((sys) => { state.system = sys; renderFirmware(); }).catch(() => {});
  }
  init();
})();
