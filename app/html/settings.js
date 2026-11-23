'use strict';

let gpsPollTimer = null;

function deviceBackupMarkup() {
  return `
    <div class="device-backup">
      <div>
        <div class="device-backup-title">Backup &amp; Restore</div>
        <div class="form-hint">Export rules and settings before uninstalling so you can restore them later.</div>
      </div>
      <div class="device-backup-actions">
        <button class="btn btn-ghost btn-sm" onclick="exportRules()">Export Rules</button>
        <button class="btn btn-ghost btn-sm" onclick="document.getElementById('import-rules-file-backup').click()">Import Rules</button>
        <button class="btn btn-ghost btn-sm" onclick="exportSettings()">Export Settings</button>
        <button class="btn btn-ghost btn-sm" onclick="document.getElementById('import-settings-file').click()">Import Settings</button>
      </div>
    </div>`;
}

async function loadStatus() {
  try {
    const s = await API.getStatus();
    const grid = document.getElementById('status-grid');
    grid.innerHTML = [
      { label: 'Rules Total',    value: s.rules_total },
      { label: 'Rules Enabled',  value: s.rules_enabled },
      { label: 'Events Today',   value: s.events_today },
    ].map(({ label, value }) => `
      <div class="stat-card">
        <div class="stat-value">${value}</div>
        <div class="stat-label">${label}</div>
      </div>
    `).join('');

    const dev = s.device || {};
    document.getElementById('device-card').innerHTML = `
      <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;">
        <h3 style="font-size:13px;color:var(--text-muted);margin:0;">DEVICE</h3>
        <button class="btn btn-ghost btn-sm" onclick="manualCheckForUpdate()">Check for Updates</button>
      </div>
      <table style="width:100%;font-size:13px;border-collapse:collapse;">
        ${[['Model', dev.model],['Serial', dev.serial],['IP', dev.ip],
           ['Firmware', dev.firmware],['Engine', s.engine_version]].map(([k, v]) => v ? `
          <tr>
            <td style="padding:5px 0;color:var(--text-muted);width:120px;">${k}</td>
            <td style="padding:5px 0;">${escHtml(v)}</td>
          </tr>` : '').join('')}
      </table>
      ${deviceBackupMarkup()}`;

    document.getElementById('header-subtitle').textContent =
      `${dev.model || ''} · ${dev.serial || ''} · ${s.rules_enabled}/${s.rules_total} rules active`;

    /* Update MQTT status badge (non-destructive — doesn't touch the form) */
    updateMqttStatusBadge(s.mqtt || {});
    updateSparkplugStatusBadge(s.sparkplug || {});
    loadSparkplugLiveMetrics();
    updateStreamStatusBadge(s);
    renderDeviceCapabilities();
    checkSerialPort();
    if (!gpsPollTimer) gpsPollTimer = setInterval(loadGpsStatus, 2000);
  } catch(e) {
    const card = document.getElementById('device-card');
    if (card) {
      card.innerHTML = `
        <h3 style="font-size:13px;color:var(--text-muted);margin:0;">DEVICE</h3>
        <div class="form-hint" style="margin-top:8px;">Device status unavailable.</div>
        ${deviceBackupMarkup()}`;
    }
    toast('Failed to load status', 'error');
  }
}

function renderDeviceCapabilities() {
  const el = document.getElementById('capabilities-card');
  if (!el) return;
  const rows = [];
  const yesNo = (v, yes) => v ? `<span style="color:var(--accent-success);">${yes}</span>` : `<span style="color:var(--text-dim);">No</span>`;

  if (ptzPresets === null) {
    rows.push(['PTZ', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else if (!ptzPresets.length) {
    rows.push(['PTZ', yesNo(false)]);
  } else {
    const total = ptzPresets.reduce((n, c) => n + c.presets.length, 0);
    rows.push(['PTZ', `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${total} preset${total !== 1 ? 's' : ''} on ${ptzPresets.length} channel${ptzPresets.length !== 1 ? 's' : ''})</span>`]);
  }

  if (audioClips === null) {
    rows.push(['Audio Clips', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else {
    rows.push(['Audio Clips', audioClips.length
      ? `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${audioClips.length} clip${audioClips.length !== 1 ? 's' : ''})</span>`
      : yesNo(false)]);
  }

  if (sirenProfiles === null) {
    rows.push(['Siren & Light', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else {
    rows.push(['Siren & Light', sirenProfiles.length
      ? `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${sirenProfiles.length} profile${sirenProfiles.length !== 1 ? 's' : ''})</span>`
      : yesNo(false)]);
  }

  if (privacyMasks !== null && privacyMasks.length) {
    rows.push(['Privacy Masks', `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${privacyMasks.length})</span>`]);
  }

  if (guardTours !== null && guardTours.length) {
    rows.push(['Guard Tours', `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${guardTours.length})</span>`]);
  }

  if (aoaScenarios === null) {
    rows.push(['Object Analytics', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else {
    rows.push(['Object Analytics (AOA)', aoaScenarios.length
      ? `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${aoaScenarios.length} scenario${aoaScenarios.length !== 1 ? 's' : ''})</span>`
      : yesNo(false)]);
  }

  if (acapApps !== null && acapApps.length) {
    const apps = acapApps.map(a => escHtml(a.niceName)).join(', ');
    rows.push(['Installed ACAPs', `<span style="font-size:11px;color:var(--text-muted);">${apps}</span>`]);
  }

  if (!rows.length) { el.style.display = 'none'; return; }
  el.style.display = '';
  el.innerHTML = `
    <h3 style="font-size:13px;color:var(--text-muted);margin-bottom:12px;">DEVICE CAPABILITIES</h3>
    <table style="width:100%;font-size:12px;border-collapse:collapse;">
      ${rows.map(([k, v]) => `<tr>
        <td style="padding:4px 0;color:var(--text-muted);width:160px;vertical-align:top;">${k}</td>
        <td style="padding:4px 0;">${v}</td>
      </tr>`).join('')}
    </table>`;
}


/* ===================================================
 * Settings tab
 * =================================================== */
function updateMqttStatusBadge(mq) {
  const dot  = document.getElementById('mqtt-dot');
  const text = document.getElementById('mqtt-status-text');
  if (!dot || !text) return;
  const suffix = mq.use_tls
    ? (mq.tls_insecure ? ' · TLS (unverified)' : mq.tls_profile ? ` · TLS (${mq.tls_profile})` : ' · TLS')
    : '';
  if (mq.connected) {
    dot.style.background = 'var(--accent-success)';
    text.textContent = `Connected — ${mq.host || ''}:${mq.port || 1883}${suffix}`;
  } else if (mq.enabled) {
    dot.style.background = 'var(--accent-warning, #f59e0b)';
    text.textContent = `Connecting…${suffix}`;
  } else {
    dot.style.background = 'var(--text-dim, #555)';
    text.textContent = 'Disabled';
  }
}

function updateStreamStatusBadge(status) {
  const dot  = document.getElementById('stream-dot');
  const text = document.getElementById('stream-status-text');
  const urlEl = document.getElementById('stream-url');
  if (!dot || !text) return;
  const clients = status.stream_clients || 0;
  const on = status.stream_enabled !== false;
  setFeatureSwitch('stream', on);
  if (!on) {
    dot.style.background = 'var(--text-dim, #555)';
    text.textContent = 'Disabled';
  } else if (clients > 0) {
    dot.style.background = 'var(--accent-success)';
    text.textContent = `${clients} client${clients !== 1 ? 's' : ''} connected`;
  } else {
    dot.style.background = 'var(--text-dim, #555)';
    text.textContent = 'No clients';
  }
  if (urlEl && status.device && status.device.ip) {
    urlEl.textContent = `http://${status.device.ip}/local/acap_event_engine/alertStream`;
  }
}

/* Mirrors engine/scheduler.c. Returns local "HH:MM", or null during polar day/night. */
function calcSolarEvent(lat, lon, eventType, offsetMin) {
  const now = new Date();
  const start = new Date(now.getFullYear(), 0, 0);
  const doy = Math.floor((now - start) / 86400000); /* 1-366 */

  const B = 2 * Math.PI * (doy - 1) / 365;
  /* Spencer formula for solar declination (radians) */
  const decl = 0.006918 - 0.399912*Math.cos(B) + 0.070257*Math.sin(B)
             - 0.006758*Math.cos(2*B) + 0.000907*Math.sin(2*B)
             - 0.002697*Math.cos(3*B) + 0.00148 *Math.sin(3*B);

  const lat_r = lat * Math.PI / 180;

  /* Equation of time (minutes) */
  const eot = 229.18 * (0.000075 + 0.001868*Math.cos(B) - 0.032077*Math.sin(B)
            - 0.014615*Math.cos(2*B) - 0.04089*Math.sin(2*B));

  const solar_noon_utc = 720 - 4*lon - eot; /* minutes UTC */

  let event_utc;
  if (eventType === 'solar_noon') {
    event_utc = solar_noon_utc;
  } else {
    const elev_r = (eventType === 'sunrise' || eventType === 'sunset')
                 ? -0.833 * Math.PI / 180   /* sunrise/sunset */
                 : -6.0   * Math.PI / 180;  /* civil dawn/dusk */
    const cos_ha = (Math.sin(elev_r) - Math.sin(lat_r) * Math.sin(decl))
                 / (Math.cos(lat_r) * Math.cos(decl));
    if (cos_ha < -1 || cos_ha > 1) return null; /* polar */
    const ha_deg = Math.acos(cos_ha) * 180 / Math.PI;
    event_utc = (eventType === 'sunrise' || eventType === 'dawn')
              ? solar_noon_utc - 4*ha_deg
              : solar_noon_utc + 4*ha_deg;
  }

  /* JS timezone offset is minutes *west*; negate to get east offset */
  const tz_offset = -now.getTimezoneOffset();
  let mins = event_utc + tz_offset + (offsetMin || 0);
  mins = ((mins % 1440) + 1440) % 1440;

  const h = Math.floor(mins / 60);
  const m = Math.floor(mins % 60);
  return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}`;
}

function refreshSolarPreview(lat, lon, containerId) {
  const el = document.getElementById(containerId);
  if (!el) return;
  if (isNaN(lat) || isNaN(lon)) { el.textContent = ''; return; }
  const events = [
    { key: 'dawn',       label: 'Civil dawn' },
    { key: 'sunrise',    label: 'Sunrise' },
    { key: 'solar_noon', label: 'Solar noon' },
    { key: 'sunset',     label: 'Sunset' },
    { key: 'dusk',       label: 'Civil dusk' },
  ];
  const parts = events.map(e => {
    const t = calcSolarEvent(lat, lon, e.key, 0);
    return t ? `<span style="margin-right:14px;"><b>${e.label}</b> ${t}</span>` : '';
  }).filter(Boolean).join('');
  el.innerHTML = parts
    ? `<div style="font-size:12px;color:var(--text-muted);margin-top:8px;">${parts}</div>`
    : `<div style="font-size:12px;color:var(--text-muted);margin-top:8px;">No sunrise/sunset at this location today (polar)</div>`;
}

function _refreshAstroTriggerPreview(rowIdx) {
  const latEl    = document.getElementById(`astro-lat-${rowIdx}`);
  const lonEl    = document.getElementById(`astro-lon-${rowIdx}`);
  const eventEl  = document.getElementById(`astro-event-${rowIdx}`);
  const offsetEl = document.getElementById(`astro-offset-${rowIdx}`);
  const preview  = document.getElementById(`astro-preview-${rowIdx}`);
  if (!latEl || !lonEl || !preview) return;
  const lat    = parseFloat(latEl.value)    || 0;
  const lon    = parseFloat(lonEl.value)    || 0;
  const ev     = eventEl  ? eventEl.value   : 'sunrise';
  const offset = offsetEl ? parseFloat(offsetEl.value) || 0 : 0;
  const t      = calcSolarEvent(lat, lon, ev, offset);
  preview.innerHTML = t
    ? `<div style="font-size:12px;color:var(--text-muted);margin-top:4px;">Today: <b>${t}</b></div>`
    : `<div style="font-size:12px;color:var(--text-muted);margin-top:4px;">No event at this location today</div>`;
}

const _astroDebounceTimers = {};
function refreshAstroTriggerPreview(rowIdx) {
  clearTimeout(_astroDebounceTimers[rowIdx]);
  _astroDebounceTimers[rowIdx] = setTimeout(() => _refreshAstroTriggerPreview(rowIdx), 300);
}

async function loadEngineSettings(settings) {
  try {
    if (!settings) settings = await API.get('settings');
    const eng = (settings && settings.engine) || {};
    engineLat = eng.latitude !== undefined ? eng.latitude : 0;
    engineLon = eng.longitude !== undefined ? eng.longitude : 0;
    const lat = document.getElementById('engine-lat');
    const lon = document.getElementById('engine-lon');
    if (lat) lat.value = engineLat;
    if (lon) lon.value = engineLon;
    refreshSolarPreview(engineLat, engineLon, 'engine-solar-preview');
  } catch(e) { /* non-fatal */ }
}

async function loadProxySettings(settings) {
  try {
    if (!settings) settings = await API.get('settings');
    const eng = (settings && settings.engine) || {};
    const proxy = document.getElementById('engine-proxy');
    if (proxy) proxy.value = eng.socks5_proxy || '';
  } catch(e) { /* non-fatal */ }
}

async function saveProxySettings(event) {
  event.preventDefault();
  const proxyEl = document.getElementById('engine-proxy');
  const proxy = proxyEl ? proxyEl.value.trim() : '';
  try {
    await postSettingsPatch('engine', { socks5_proxy: proxy });
    refreshSectionSummary('proxy');
    toast(proxy ? 'Proxy settings saved' : 'Proxy cleared');
  } catch(e) {
    toast('Failed to save proxy settings: ' + e.message, 'error');
  }
}

async function saveEngineSettings(event) {
  event.preventDefault();
  const lat = parseFloat(document.getElementById('engine-lat').value) || 0;
  const lon = parseFloat(document.getElementById('engine-lon').value) || 0;
  try {
    await postSettingsPatch('engine', { latitude: lat, longitude: lon });
    engineLat = lat;
    engineLon = lon;
    refreshSectionSummary('location');
    toast('Location saved');
    refreshSolarPreview(lat, lon, 'engine-solar-preview');
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}

async function loadSmtpSettings(settings) {
  try {
    if (!settings) settings = await API.get('settings');
    const smtp = (settings && settings.smtp) || {};
    const el = (id) => document.getElementById(id);
    if (el('smtp-server')) el('smtp-server').value = smtp.server || '';
    if (el('smtp-user'))   el('smtp-user').value   = smtp.username || '';
    if (el('smtp-pass'))   el('smtp-pass').value   = '';  /* never show stored pw */
    if (el('smtp-from'))   el('smtp-from').value   = smtp.from || '';
      if (el('smtp-tls'))    el('smtp-tls').checked   = smtp.use_tls !== false;
    if (el('smtp-tls-insecure')) el('smtp-tls-insecure').checked = !!smtp.tls_insecure;
    if (el('smtp-tls-profile')) {
      el('smtp-tls-profile').dataset.value = smtp.tls_profile || '';
      fillTlsProfileSelect(el('smtp-tls-profile'));
    }
  } catch(e) { /* non-fatal */ }
}

async function saveSmtpSettings(event) {
  event.preventDefault();
  const el = (id) => document.getElementById(id);
  const data = {
    server:   el('smtp-server') ? el('smtp-server').value.trim() : '',
    username: el('smtp-user')   ? el('smtp-user').value.trim()   : '',
    from:     el('smtp-from')   ? el('smtp-from').value.trim()   : '',
      use_tls:  el('smtp-tls')    ? !!el('smtp-tls').checked : true,
    tls_insecure: el('smtp-tls-insecure') ? !!el('smtp-tls-insecure').checked : false,
    tls_profile:  el('smtp-tls-profile') ? el('smtp-tls-profile').value : '',
  };
  const pw = el('smtp-pass') ? el('smtp-pass').value : '';
  if (pw) data.password = pw;
  try {
    await API.post('settings', { smtp: data });
    if (_savedSettings) _savedSettings.smtp = Object.assign({}, data, { password: '' });
    refreshSectionSummary('smtp');
    toast('SMTP settings saved');
    if (el('smtp-pass')) el('smtp-pass').value = '';
    if (el('smtp-tls-profile')) el('smtp-tls-profile').dataset.value = data.tls_profile;
    loadTlsProfiles();
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}

async function loadMqttSettings(settings) {
  try {
    if (!settings) settings = await API.get('settings');
    const mq = (settings && settings.mqtt) || {};
    const form = document.getElementById('mqtt-form');
    if (!form) return;
    form.querySelector('[name="host"]').value      = mq.host      || '';
    form.querySelector('[name="port"]').value      = mq.port      || 1883;
    form.querySelector('[name="client_id"]').value = mq.client_id || 'acap_event_engine';
    form.querySelector('[name="username"]').value  = mq.username  || '';
    form.querySelector('[name="keepalive"]').value = mq.keepalive || 60;
    form.querySelector('[name="use_tls"]').checked = !!mq.use_tls;
    form.querySelector('[name="tls_insecure"]').checked = !!mq.tls_insecure;
    form.querySelector('[name="tls_profile"]').dataset.value = mq.tls_profile || '';
    fillSettingsTlsSelects();
    mqttTlsRowVisibility();
    setFeatureSwitch('mqtt', mq.enabled);
    initSectionOpen('mqtt', !!mq.enabled);
    form.querySelector('[name="servers"]').value = (mq.servers || []).join('\n');
    /* Don't pre-fill password — leave placeholder "(unchanged)" */

    const status = await API.getStatus();
    updateMqttStatusBadge((status && status.mqtt) || {});
  } catch(e) {
    toast('Failed to load MQTT settings', 'error');
  }
}

async function saveMqttSettings(event) {
  event.preventDefault();
  const form = document.getElementById('mqtt-form');
  const pw = form.querySelector('[name="password"]').value;
  const payload = {
    mqtt: {
      enabled:   form.elements.enabled.checked,
      host:      form.querySelector('[name="host"]').value.trim(),
      port:      parseInt(form.querySelector('[name="port"]').value, 10) || 1883,
      client_id: form.querySelector('[name="client_id"]').value.trim(),
      username:  form.querySelector('[name="username"]').value.trim(),
      keepalive: parseInt(form.querySelector('[name="keepalive"]').value, 10) || 60,
      use_tls:   form.querySelector('[name="use_tls"]').checked,
      tls_insecure: form.querySelector('[name="tls_insecure"]').checked,
      tls_profile: form.querySelector('[name="tls_profile"]').value,
      servers:   form.querySelector('[name="servers"]').value
                     .split('\n').map(s => s.trim()).filter(Boolean).slice(0, 7)
    }
  };
  /* Only include password if user typed something */
  if (pw) payload.mqtt.password = pw;
  try {
    const r = await API.post('settings', payload);
    toast('MQTT settings saved');
    form.querySelector('[name="password"]').value = '';
    form.querySelector('[name="tls_profile"]').dataset.value = payload.mqtt.tls_profile;
    loadTlsProfiles();
    /* Refresh status badge after a short delay to let the engine reconnect */
    setTimeout(loadMqttSettings, 1500);
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}

/* ===== Sparkplug B ===== */
const SPB_DATATYPES = ['Boolean','Int32','Int64','UInt32','UInt64','Float','Double','String','DateTime','Text'];

let sparkplugMetrics = [];

function updateSparkplugStatusBadge(sp) {
  const dot  = document.getElementById('spb-dot');
  const text = document.getElementById('spb-status-text');
  if (!dot || !text) return;

  if (!sp || !sp.enabled) {
    dot.style.background = 'var(--text-dim)';
    text.textContent = 'Disabled';
    return;
  }
  if (sp.online) {
    dot.style.background = 'var(--accent-success)';
    const buffered = sp.buffered ? ` · ${sp.buffered} buffered` : '';
    text.textContent = `Online · bdSeq ${sp.bdseq} · seq ${sp.seq} · ${sp.metric_count} metrics${buffered}`;
  } else {
    dot.style.background = 'var(--accent-warning)';
    text.textContent = sp.buffered ? `Offline · ${sp.buffered} samples buffered` : 'Offline';
  }
}

/* The SOAP catalog only prefixes levels where the XML declares a namespace, while saved
 * config qualifies every level, so compare local names (as the event system does). */
function normalizeTopicPath(path) {
  return String(path || '')
    .split('/')
    .map(seg => { const i = seg.indexOf(':'); return i > 0 ? seg.slice(i + 1) : seg; })
    .filter(Boolean)
    .join('/');
}

function sparkplugCatalogIndex(m) {
  if (!m._sourcePath || !Array.isArray(vapixEventCatalog)) return -1;
  const want = normalizeTopicPath(m._sourcePath);
  return vapixEventCatalog.findIndex(ev => normalizeTopicPath(vapixCatalogTopicPath(ev)) === want);
}

function topicPathFromSource(source) {
  return ['topic0','topic1','topic2','topic3']
    .filter(k => source && source[k])
    .map(k => {
      const [ns, val] = Object.entries(source[k])[0] || ['', ''];
      return ns ? `${ns}:${val}` : val;
    })
    .join('/');
}

/* "tnsaxis:CameraApplicationPlatform/tnsaxis:ObjectAnalytics/..." -> topic0..3.
 * A segment without a namespace defaults to tns1 at the root and tnsaxis below,
 * which matches how Axis names its standard and vendor topics. */
function sourceFromTopicPath(path) {
  const parts = String(path || '').split('/').map(s => s.trim()).filter(Boolean).slice(0, 4);
  if (!parts.length) return null;
  const source = {};
  parts.forEach((part, i) => {
    const sep  = part.indexOf(':');
    const ns   = sep > 0 ? part.slice(0, sep) : (i === 0 ? 'tns1' : 'tnsaxis');
    const name = sep > 0 ? part.slice(sep + 1) : part;
    source[`topic${i}`] = { [ns]: name };
  });
  return source;
}

/* The SOAP catalog omits CameraApplicationPlatform topics, so offer "keep" for
 * unlisted bindings and a way to type a topic path directly. */
function sparkplugEventOptions(m) {
  const bound  = !!m.source;
  const catIdx = sparkplugCatalogIndex(m);
  const manual = !!m._manual;
  const opts   = [`<option value="-1" ${(!bound && !manual) ? 'selected' : ''}>Not bound (rule-driven)</option>`];

  if (bound && catIdx < 0 && !manual)
    opts.push(`<option value="keep" selected>${escHtml(normalizeTopicPath(m._sourcePath) || 'current binding')} (not in catalog)</option>`);

  opts.push(`<option value="manual" ${manual ? 'selected' : ''}>Enter a topic path manually…</option>`);

  if (Array.isArray(vapixEventCatalog) && vapixEventCatalog.length) {
    vapixEventCatalog.forEach((ev, i) => {
      opts.push(`<option value="${i}" ${(!manual && i === catIdx) ? 'selected' : ''}>${escHtml(ev.label)}</option>`);
    });
  } else {
    opts.push('<option value="-2" disabled>(loading device event list…)</option>');
  }
  return opts.join('');
}

function sparkplugMetricSummary(m) {
  const name = m.name ? escHtml(m.name) : '<em style="opacity:.6">unnamed metric</em>';
  const bind = m.source
    ? `bound to ${escHtml(normalizeTopicPath(m._sourcePath) || 'event')}${m.source_key ? ' · ' + escHtml(m.source_key) : ''}`
    : 'rule-driven';
  return `<strong>${name}</strong>
          <span style="color:var(--text-muted);font-size:12px;margin-left:8px;">
            ${escHtml(m.datatype || 'String')} · ${bind}
          </span>`;
}

function renderSparkplugMetrics() {
  const el = document.getElementById('spb-metrics');
  if (!el) return;

  if (!sparkplugMetrics.length) {
    el.innerHTML = '<div class="form-hint">No metrics declared yet.</div>';
    return;
  }

  el.innerHTML = sparkplugMetrics.map((m, i) => {
    const header = `
      <div style="display:flex;align-items:center;gap:8px;">
        <button type="button" class="btn btn-ghost btn-sm" onclick="toggleSparkplugMetric(${i})"
                style="flex:0 0 auto;width:28px;" title="${m._open ? 'Collapse' : 'Expand'}">${m._open ? '▾' : '▸'}</button>
        <div style="flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:pointer;"
             onclick="toggleSparkplugMetric(${i})">${sparkplugMetricSummary(m)}</div>
        <button type="button" class="btn btn-ghost btn-sm" onclick="removeSparkplugMetric(${i})"
                style="flex:0 0 auto;">Remove</button>
      </div>`;

    if (!m._open)
      return `<div style="padding:8px 10px;border:1px solid var(--border);border-radius:6px;margin-bottom:6px;">${header}</div>`;

    const catIdx = sparkplugCatalogIndex(m);
    const keys = (catIdx >= 0 && vapixEventCatalog[catIdx].dataKeys) || [];
    const keyField = keys.length
      ? `<select data-spb="source_key" data-i="${i}">${
           keys.map(k => `<option value="${escHtml(k)}" ${k === m.source_key ? 'selected' : ''}>${escHtml(k)}</option>`).join('')
         }</select>`
      : `<input type="text" data-spb="source_key" data-i="${i}" value="${escHtml(m.source_key || '')}" placeholder="state">`;

    return `
    <div style="padding:8px 10px;border:1px solid var(--border);border-radius:6px;margin-bottom:6px;">
      ${header}
      <div style="margin-top:10px;">
        <div class="form-row">
          <div class="form-group" style="flex:2 1 220px;">
            <label>Metric name</label>
            <input type="text" data-spb="name" data-i="${i}" value="${escHtml(m.name || '')}"
                   placeholder="Speaker/Motion" oninput="refreshSparkplugSummary(${i})">
          </div>
          <div class="form-group" style="flex:1 1 130px;">
            <label>Type</label>
            <select data-spb="datatype" data-i="${i}" onchange="refreshSparkplugSummary(${i})">
              ${SPB_DATATYPES.map(d => `<option value="${d}" ${d === (m.datatype || 'String') ? 'selected' : ''}>${d}</option>`).join('')}
            </select>
          </div>
        </div>
        <div class="form-row">
          <div class="form-group" style="flex:2 1 260px;">
            <label>Bind to device event</label>
            <select data-spb="catalog" data-i="${i}" onchange="applySparkplugMetricEvent(${i}, this.value)">
              ${sparkplugEventOptions(m)}
            </select>
            ${m._manual ? `
            <input type="text" data-spb="_pathInput" data-i="${i}" style="margin-top:6px;"
                   value="${escHtml(m._sourcePath || '')}"
                   placeholder="tnsaxis:CameraApplicationPlatform/tnsaxis:ObjectAnalytics/tnsaxis:Device1Scenario1"
                   onchange="applySparkplugManualPath(${i}, this.value)">
            <div class="form-hint">Slash-separated topic levels. Use this for events the
            dropdown does not list, such as ACAP events
            (<code>tnsaxis:CameraApplicationPlatform/…</code>).</div>` : `
            <div class="form-hint">Leave unbound to drive this metric from a Sparkplug B Publish action.</div>`}
          </div>
          <div class="form-group" style="flex:1 1 160px;">
            <label>Field</label>
            ${keyField}
            <div class="form-hint">Event field to read.</div>
          </div>
        </div>
      </div>
    </div>`;
  }).join('');
}

function collectSparkplugMetrics() {
  document.querySelectorAll('#spb-metrics [data-spb]').forEach(inp => {
    const i = parseInt(inp.dataset.i, 10);
    const k = inp.dataset.spb;
    if (isNaN(i) || !sparkplugMetrics[i] || k === 'catalog') return;
    sparkplugMetrics[i][k] = inp.value;
  });
}

function toggleSparkplugMetric(i) {
  collectSparkplugMetrics();
  if (!sparkplugMetrics[i]) return;
  sparkplugMetrics[i]._open = !sparkplugMetrics[i]._open;
  renderSparkplugMetrics();
}

/* Update the collapsed summary without re-rendering (which would drop focus) */
function refreshSparkplugSummary(i) {
  collectSparkplugMetrics();
  const row = document.querySelector(`#spb-metrics [data-spb="name"][data-i="${i}"]`);
  const head = row && row.closest('div[style*="border"]').querySelector('div[onclick^="toggleSparkplugMetric"]');
  if (head) head.innerHTML = sparkplugMetricSummary(sparkplugMetrics[i]);
}

function applySparkplugMetricEvent(i, value) {
  collectSparkplugMetrics();
  const m = sparkplugMetrics[i];
  if (!m) return;

  if (value === 'keep') return;   /* binding kept exactly as loaded */

  if (value === 'manual') {
    m._manual = true;
    renderSparkplugMetrics();
    return;
  }

  delete m._manual;
  const idx = parseInt(value, 10);
  const ev = (idx >= 0 && vapixEventCatalog) ? vapixEventCatalog[idx] : null;
  if (!ev) {
    delete m.source;
    delete m._sourcePath;
  } else {
    m.source = ev.topics;
    m._sourcePath = vapixCatalogTopicPath(ev);
    if (!ev.dataKeys.includes(m.source_key))
      m.source_key = ev.dataKeys.length ? ev.dataKeys[0] : 'state';
  }
  renderSparkplugMetrics();
}

function applySparkplugManualPath(i, path) {
  collectSparkplugMetrics();
  const m = sparkplugMetrics[i];
  if (!m) return;

  const source = sourceFromTopicPath(path);
  if (!source) {
    delete m.source;
    delete m._sourcePath;
  } else {
    m.source = source;
    m._sourcePath = topicPathFromSource(source);
    if (!m.source_key) m.source_key = 'state';
  }
  renderSparkplugMetrics();
}

function addSparkplugMetric() {
  collectSparkplugMetrics();
  sparkplugMetrics.push({ name: '', datatype: 'String', source_key: '', _open: true });
  renderSparkplugMetrics();
}

function removeSparkplugMetric(i) {
  collectSparkplugMetrics();
  sparkplugMetrics.splice(i, 1);
  renderSparkplugMetrics();
}

async function loadSparkplugSettings(settings) {
  try {
    if (!settings) settings = await API.get('settings');
    const sp = (settings && settings.sparkplug) || {};
    const form = document.getElementById('spb-form');
    if (!form) return;

    setFeatureSwitch('sparkplug', sp.enabled);
    initSectionOpen('sparkplug', !!sp.enabled);
    form.querySelector('[name="spec_version"]').value    = sp.spec_version || '3.0';
    form.querySelector('[name="group_id"]').value        = sp.group_id || '';
    form.querySelector('[name="edge_node_id"]').value    = sp.edge_node_id || '';
    form.querySelector('[name="device_id"]').value       = sp.device_id || '';
    form.querySelector('[name="primary_host_id"]').value = sp.primary_host_id || '';

    sparkplugMetrics = (sp.metrics || []).map(m => {
      const copy = { name: m.name || '', datatype: m.datatype || 'String', source_key: m.source_key || '' };
      if (m.source && Object.keys(m.source).length) {
        copy.source = m.source;
        copy._sourcePath = topicPathFromSource(m.source);
      }
      return copy;
    });
    renderSparkplugMetrics();

    /* The event catalog loads asynchronously; redraw once it arrives so bound
     * metrics resolve to their catalog entry instead of the raw topic path. */
    if (vapixEventCatalog === null && typeof loadVapixEventCatalog === 'function')
      loadVapixEventCatalog().then(renderSparkplugMetrics).catch(() => {});

    const status = await API.getStatus();
    updateSparkplugStatusBadge((status && status.sparkplug) || {});
    loadSparkplugLiveMetrics();
  } catch(e) {
    toast('Failed to load Sparkplug settings', 'error');
  }
}

/* Live metric values straight from the running edge node */
async function loadSparkplugLiveMetrics() {
  const el = document.getElementById('spb-live');
  if (!el) return;
  try {
    const data = await API.get('sparkplug');
    const metrics = (data && data.metrics) || [];
    if (!metrics.length) { el.innerHTML = ''; return; }

    el.innerHTML = `
      <h4 style="font-size:12px;color:var(--text-muted);margin:16px 0 8px;">LIVE METRIC VALUES</h4>
      <table style="width:100%;font-size:13px;border-collapse:collapse;">
        <tr style="color:var(--text-muted);text-align:left;">
          <th style="padding:4px 8px 4px 0;font-weight:500;">Metric</th>
          <th style="padding:4px 8px;font-weight:500;width:80px;">Alias</th>
          <th style="padding:4px 8px;font-weight:500;width:90px;">Type</th>
          <th style="padding:4px 8px;font-weight:500;width:80px;">Source</th>
          <th style="padding:4px 0;font-weight:500;">Value</th>
        </tr>
        ${metrics.map(m => `
          <tr style="border-top:1px solid var(--border);">
            <td style="padding:5px 8px 5px 0;">${escHtml(m.name)}</td>
            <td style="padding:5px 8px;color:var(--text-muted);">@${m.alias}</td>
            <td style="padding:5px 8px;color:var(--text-muted);">${escHtml(m.datatype)}</td>
            <td style="padding:5px 8px;color:var(--text-muted);">${m.auto ? 'event' : 'rule'}</td>
            <td style="padding:5px 0;">${m.value === null || m.value === undefined
                ? '<span style="color:var(--text-dim);">null</span>'
                : escHtml(String(m.value))}</td>
          </tr>`).join('')}
      </table>`;
  } catch(e) {
    el.innerHTML = '';
  }
}

async function saveSparkplugSettings(event) {
  event.preventDefault();
  collectSparkplugMetrics();

  const form = document.getElementById('spb-form');
  const enabled = form.elements.enabled.checked;
  const groupId = form.querySelector('[name="group_id"]').value.trim();

  if (enabled && !groupId) { toast('Group ID is required', 'error'); return; }

  const bad = /[\/+#]/;
  for (const [label, name] of [['Group ID','group_id'], ['Edge Node ID','edge_node_id'],
                               ['Device ID','device_id'], ['Primary Host ID','primary_host_id']]) {
    const v = form.querySelector(`[name="${name}"]`).value.trim();
    if (v && bad.test(v)) { toast(`${label} must not contain / + or #`, 'error'); return; }
  }

  const seen = new Set();
  const metrics = [];
  let unnamed = 0;
  for (const m of sparkplugMetrics) {
    const name = (m.name || '').trim();
    if (!name) { unnamed++; continue; }
    if (seen.has(name)) { toast(`Duplicate metric name: ${name}`, 'error'); return; }
    seen.add(name);
    const out = { name, datatype: m.datatype || 'String' };
    if (m.source) { out.source = m.source; out.source_key = (m.source_key || 'state').trim(); }
    metrics.push(out);
  }
  if (unnamed) toast(`${unnamed} metric row(s) without a name were dropped`, 'warning');

  const payload = {
    sparkplug: {
      enabled,
      spec_version:    form.querySelector('[name="spec_version"]').value,
      group_id:        groupId,
      edge_node_id:    form.querySelector('[name="edge_node_id"]').value.trim(),
      device_id:       form.querySelector('[name="device_id"]').value.trim(),
      primary_host_id: form.querySelector('[name="primary_host_id"]').value.trim(),
      metrics
    }
  };

  try {
    await API.post('settings', payload);
    toast('Sparkplug settings saved');
    /* Give the edge node time to reconnect and rebirth before reading status */
    setTimeout(() => loadSparkplugSettings(), 2000);
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}

/* Fetch /settings once and populate all settings sections */
async function loadAllSettings() {
  try {
    const settings = await API.get('settings');
    _savedSettings = settings || {};
    initOptionalSections();
    loadEngineSettings(settings);
    loadProxySettings(settings);
    loadSmtpSettings(settings);
    loadMqttSettings(settings);
    loadSparkplugSettings(settings);
    loadGpsSettings(settings);
    loadTlsProfiles();
    ['smtp', 'location', 'proxy'].forEach(refreshSectionSummary);
    initSectionOpen('stream', (settings.engine || {}).alert_stream_enabled !== false);
  } catch(e) { /* non-fatal — individual loaders will show errors as needed */ }
}

/* ===== Optional settings sections =====
 * Clicking a section heading only shows or hides it (remembered per browser).
 * Features that run in the background (MQTT, Sparkplug, GPS, Alert Stream,
 * RS-485) also have an on/off switch in the heading that changes the setting
 * right away. A section starts open when it is in use. */
let _savedSettings = null;
let _serialModeRS485 = false;
const _sectionInitDone = {};
const _SECTION_OPEN_KEY = 'eventEngine.settingsOpen';

const _FEATURE_LABELS = {
  mqtt:      'MQTT',
  sparkplug: 'Sparkplug B',
  gps:       'GPS input',
  stream:    'the Alert Stream',
  serial:    'the RS-485 gateway',
};

function _sectionCard(id) {
  return document.querySelector(`.opt-section[data-section="${id}"]`);
}

function _sectionOpenPrefs() {
  try { return JSON.parse(localStorage.getItem(_SECTION_OPEN_KEY)) || {}; } catch(_) { return {}; }
}

function setSectionOpen(id, open, remember) {
  const card = _sectionCard(id);
  if (!card) return;
  card.classList.toggle('open', !!open);
  const btn = card.querySelector('.section-chevron');
  if (btn) btn.setAttribute('aria-expanded', open ? 'true' : 'false');
  if (remember) {
    const prefs = _sectionOpenPrefs();
    prefs[id] = !!open;
    try { localStorage.setItem(_SECTION_OPEN_KEY, JSON.stringify(prefs)); } catch(_) {}
  }
}

/* Sets the initial open state once per page load, so status polling never
 * opens or closes a section under the user. */
function initSectionOpen(id, inUse) {
  if (_sectionInitDone[id]) return;
  _sectionInitDone[id] = true;
  const pref = _sectionOpenPrefs()[id];
  setSectionOpen(id, typeof pref === 'boolean' ? pref : inUse);
}

function setFeatureSwitch(id, on) {
  const t = _sectionCard(id) && _sectionCard(id).querySelector('.section-head .toggle input');
  if (t) t.checked = !!on;
}

function initOptionalSections() {
  document.querySelectorAll('.opt-section').forEach(card => {
    const head = card.querySelector('.section-head');
    if (!head || head.dataset.ready) return;
    head.dataset.ready = '1';
    const id = card.dataset.section;
    const title = card.querySelector('.section-title');
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'section-chevron';
    btn.setAttribute('aria-expanded', card.classList.contains('open') ? 'true' : 'false');
    btn.setAttribute('aria-label', 'Show or hide ' + (title ? title.textContent : id));
    btn.innerHTML = '<svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6l4 4 4-4"/></svg>';
    head.appendChild(btn);
    head.addEventListener('click', e => {
      if (e.target.closest('.toggle, a')) return;
      setSectionOpen(id, !card.classList.contains('open'), true);
    });
  });
}

function _setSummary(id, text, on) {
  const el = document.getElementById(id + '-summary');
  if (!el) return;
  el.textContent = text;
  el.style.color = on ? 'var(--text)' : '';
}

function refreshSectionSummary(id) {
  const s = _savedSettings || {};
  const eng = s.engine || {};
  let inUse = false;
  if (id === 'smtp') {
    const server = (s.smtp || {}).server || '';
    inUse = !!server;
    _setSummary(id, server || 'Not set up', inUse);
  } else if (id === 'location') {
    const lat = Number(eng.latitude) || 0, lon = Number(eng.longitude) || 0;
    inUse = !!(lat || lon);
    _setSummary(id, inUse ? `${lat.toFixed(4)}, ${lon.toFixed(4)}` : 'Not set', inUse);
  } else if (id === 'proxy') {
    inUse = !!eng.socks5_proxy;
    _setSummary(id, eng.socks5_proxy || 'Direct connection', inUse);
  } else if (id === 'tls') {
    const n = Array.isArray(tlsProfiles) ? tlsProfiles.length : 0;
    inUse = n > 0;
    _setSummary(id, n ? `${n} profile${n !== 1 ? 's' : ''}` : 'No profiles', inUse);
  }
  initSectionOpen(id, inUse);
}

/* POST replaces a whole top-level settings object, so merge with what is
 * stored to avoid dropping sibling keys (e.g. engine.latitude vs socks5_proxy). */
async function postSettingsPatch(key, patch) {
  const cur = (await API.get('settings')) || {};
  const merged = Object.assign({}, cur[key] || {}, patch);
  await API.post('settings', { [key]: merged });
  cur[key] = merged;
  _savedSettings = cur;
  return merged;
}

/* A feature that needs fields filled in before it can run is switched on by
 * its Save button instead; the switch then only opens the form. */
function _featureNeedsSetup(id) {
  const s = _savedSettings || {};
  if (id === 'mqtt')      return !(s.mqtt && s.mqtt.host);
  if (id === 'sparkplug') return !(s.sparkplug && s.sparkplug.group_id);
  return false;
}

async function onFeatureToggle(id, input) {
  const on = input.checked;
  const label = _FEATURE_LABELS[id];
  const Label = label.charAt(0).toUpperCase() + label.slice(1);
  try { _savedSettings = (await API.get('settings')) || _savedSettings; } catch(_) {}

  if (on && _featureNeedsSetup(id)) {
    input.checked = false;
    setSectionOpen(id, true, true);
    const first = _sectionCard(id).querySelector('.section-body input:not([type="checkbox"]):not([type="file"])');
    if (first) first.focus();
    toast(`Configure and save ${label} before turning it on`);
    return;
  }

  if (id === 'serial') {
    if (on) {
      const baud = document.getElementById('serial-baud').value;
      const port = document.getElementById('serial-port').value;
      const message = _serialModeRS485
        ? `Enable the existing RS-485 gateway on TCP port ${port}? Serial settings will not be changed.`
        : `Set the serial port to RS-485 at ${baud} baud and open TCP port ${port} for it?`;
      if (!confirm(message)) { input.checked = false; return; }
      setSectionOpen(id, true, true);
      const succeeded = _serialModeRS485 ? await enableSerialListener() : await configureSerial();
      if (!succeeded) await checkSerialPort();
    } else {
      if (!confirm('Turn off the RS-485 gateway? The serial port settings are kept.')) { input.checked = true; return; }
      await disableSerial();
    }
    return;
  }

  if (!on && !confirm(`Turn off ${label}? Its settings are kept so you can turn it back on later.`)) {
    input.checked = true;
    return;
  }

  try {
    if (id === 'stream') await postSettingsPatch('engine', { alert_stream_enabled: on });
    else                 await postSettingsPatch(id, { enabled: on });
    if (on) setSectionOpen(id, true, true);
    toast(`${Label} turned ${on ? 'on' : 'off'}`);
    if (id === 'mqtt')      setTimeout(() => API.getStatus().then(s => updateMqttStatusBadge((s && s.mqtt) || {})).catch(() => {}), 1500);
    if (id === 'sparkplug') setTimeout(() => API.getStatus()
      .then(s => updateSparkplugStatusBadge((s && s.sparkplug) || {})).catch(() => {}), 1500);
    if (id === 'gps')       setTimeout(loadGpsStatus, 500);
    if (id === 'stream')    API.getStatus().then(updateStreamStatusBadge).catch(() => {});
  } catch(e) {
    input.checked = !on;
    toast(`Failed to turn ${on ? 'on' : 'off'}: ` + e.message, 'error');
  }
}

function gpsSourceVisibility() {
  const form = document.getElementById('gps-form');
  if (!form) return;
  const source = form.elements.source.value;
  const serial = source === 'serial';
  document.getElementById('gps-serial-row').style.display = serial ? 'flex' : 'none';
  document.getElementById('gps-network-row').style.display = serial ? 'none' : 'flex';
  document.getElementById('gps-host-group').style.display = source === 'tcp_client' ? 'block' : 'none';
  document.getElementById('gps-port-label').textContent =
    source === 'tcp_client' ? 'GPS server port' : 'Listen port';
}

function loadGpsSettings(settings) {
  const cfg = (settings && settings.gps) || {};
  const form = document.getElementById('gps-form');
  if (!form) return;
  setFeatureSwitch('gps', cfg.enabled);
  initSectionOpen('gps', !!cfg.enabled);
  form.elements.source.value = cfg.source || 'serial';
  form.elements.device.value = cfg.device || 'auto';
  form.elements.baud.value = String(cfg.baud === undefined ? 0 : cfg.baud);
  form.elements.host.value = cfg.host || '127.0.0.1';
  form.elements.port.value = cfg.port || 5010;
  form.elements.stale_after_s.value = cfg.stale_after_s || 5;
  gpsSourceVisibility();
  loadGpsStatus();
}

async function saveGpsSettings(event) {
  event.preventDefault();
  const form = event.currentTarget;
  const cfg = {
    enabled: form.elements.enabled.checked,
    source: form.elements.source.value,
    device: form.elements.device.value.trim() || 'auto',
    baud: parseInt(form.elements.baud.value, 10) || 0,
    host: form.elements.host.value.trim() || '127.0.0.1',
    port: parseInt(form.elements.port.value, 10),
    stale_after_s: parseInt(form.elements.stale_after_s.value, 10)
  };
  if (!cfg.port || cfg.port < 1 || cfg.port > 65535 ||
      !cfg.stale_after_s || cfg.stale_after_s < 1 || cfg.stale_after_s > 3600) {
    toast('Check the GPS port and stale timeout values', 'error');
    return;
  }
  try {
    await API.post('settings', { gps: cfg });
    toast('GPS settings saved', 'success');
    setTimeout(loadGpsStatus, 500);
  } catch(e) {
    toast('Failed to save GPS settings: ' + e.message, 'error');
  }
}

async function loadGpsStatus() {
  try {
    const s = await API.get('gps');
    const dot = document.getElementById('gps-dot');
    const status = document.getElementById('gps-status-text');
    const live = document.getElementById('gps-live-status');
    const raw = document.getElementById('gps-raw-lines');
    if (!dot || !status || !live || !raw) return;
    const color = !s.enabled ? 'var(--text-dim)'
      : (s.has_fix && s.fresh) ? 'var(--accent-success)'
      : s.state === 'error' ? '#ef4444' : '#f59e0b';
    dot.style.background = color;
    status.textContent = !s.enabled ? 'Disabled' :
      `${s.state}${s.has_fix && s.fresh ? ' · fix' : ''}`;
    const position = s.has_fix && s.fresh
      ? `${Number(s.latitude).toFixed(6)}, ${Number(s.longitude).toFixed(6)}`
      : '--';
    const speed = s.has_fix && s.fresh && Number.isFinite(Number(s.speed_kmh))
      ? `${Number(s.speed_kmh).toFixed(1)} km/h` : '--';
    live.innerHTML = [
      `Position: <b>${escHtml(position)}</b>`,
      `Speed: <b>${escHtml(speed)}</b>`,
      `Satellites: <b>${escHtml(s.satellites)}</b>`,
      `Bytes / lines / parsed: <b>${escHtml(s.bytes_received)} / ${escHtml(s.lines_received)} / ${escHtml(s.sentences_parsed)}</b>`,
      `Checksum / format errors: <b>${escHtml(s.checksum_errors)} / ${escHtml(s.format_errors)}</b>`,
      s.last_error ? `Last error: <b>${escHtml(s.last_error)}</b>` : ''
    ].filter(Boolean).join(' &nbsp;·&nbsp; ');
    raw.textContent = (s.raw_lines || []).join('\n');
  } catch(e) {
    const status = document.getElementById('gps-status-text');
    if (status) status.textContent = 'Status unavailable';
  }
}

/* ===== TLS profiles (certificates for HTTPS and MQTT) ===== */

async function loadTlsProfiles() {
  try {
    tlsProfiles = await API.getCerts();
  } catch(e) {
    if (!Array.isArray(tlsProfiles)) tlsProfiles = [];
  }
  renderTlsProfiles();
  fillSettingsTlsSelects();
  refreshSectionSummary('tls');
  return tlsProfiles;
}

function apiErrorText(e) {
  return String((e && e.message) || e).replace(/^HTTP \d+:\s*/, '');
}

function cnOf(dn) {
  const m = /(?:^|,)CN=((?:\\,|[^,])+)/.exec(dn || '');
  return m ? m[1].replace(/\\,/g, ',') : (dn || '');
}

function fmtCertDate(epoch) {
  return new Date(epoch * 1000).toISOString().slice(0, 10);
}

function tlsExpiryBadge(expires) {
  if (!expires) return '<span style="color:var(--text-muted);">—</span>';
  const days = Math.floor((expires * 1000 - Date.now()) / 86400000);
  const nw = 'style="white-space:nowrap;"';
  if (days < 0)  return `<span class="badge event-badge-error" ${nw}>Expired ${fmtCertDate(expires)}</span>`;
  if (days < 30) return `<span class="badge event-badge-warning" ${nw}>Expires in ${days} day${days === 1 ? '' : 's'}</span>`;
  return `<span class="badge event-badge-fired" ${nw}>Valid until ${fmtCertDate(expires)}</span>`;
}

function renderTlsProfiles() {
  const box = document.getElementById('tls-list');
  if (!box) return;
  const list = Array.isArray(tlsProfiles) ? tlsProfiles : [];
  if (!list.length) {
    box.innerHTML = `<div style="font-size:13px;color:var(--text-muted);">No profiles yet.</div>`;
    return;
  }
  box.innerHTML = `<table class="var-table">
    <thead><tr><th>Name</th><th>Client certificate</th><th>Server check</th><th>Expiry</th><th>Used by</th><th></th></tr></thead>
    <tbody>${list.map(p => {
      const leaf = p.cert && p.cert[0];
      const client = leaf
        ? `${escHtml(leaf.cn || leaf.subject)}<div class="form-hint">issued by ${escHtml(cnOf(leaf.issuer))}${p.key ? ` · ${escHtml(p.key.type)} ${p.key.bits}-bit` : ''}</div>`
        : '<span style="color:var(--text-muted);">None</span>';
      const server = p.insecure
        ? '<span class="badge event-badge-warning" style="white-space:nowrap;">Not verified</span>'
        : p.ca ? `Trusts ${escHtml(p.ca.map(c => c.cn || cnOf(c.subject)).join(', '))}`
               : '<span style="color:var(--text-muted);">Public CAs</span>';
      const used = (p.used_by || []).map(u => escHtml(u.name)).join(', ')
        || '<span style="color:var(--text-muted);">Not used</span>';
      return `<tr>
        <td><b>${escHtml(p.name)}</b></td><td>${client}</td><td>${server}</td>
        <td>${tlsExpiryBadge(p.expires)}</td><td>${used}</td>
        <td style="white-space:nowrap;text-align:right;">
          <button class="btn btn-ghost btn-sm" data-name="${escHtml(p.name)}" onclick="tlsEditProfile(this.dataset.name)">Edit</button>
          <button class="btn btn-ghost btn-sm" data-name="${escHtml(p.name)}" onclick="tlsDeleteProfile(this.dataset.name)">Delete</button>
        </td></tr>`;
    }).join('')}</tbody></table>`;
}

let tlsEditing = null;   /* null = editor closed, '' = new profile, otherwise the profile name */

function tlsPemField(part, label, hint, current) {
  const begin = part === 'key' ? 'PRIVATE KEY' : 'CERTIFICATE';
  return `<div class="form-row"><div class="form-group">
    <label>${label}</label>
    ${current ? `<div style="font-size:12px;margin-bottom:6px;">Current: ${current}
      <label style="display:inline-flex;align-items:center;gap:6px;margin-left:12px;font-weight:400;cursor:pointer;">
        <input type="checkbox" id="tls-remove-${part}"> Remove</label></div>` : ''}
    <div style="display:flex;gap:8px;align-items:center;margin-bottom:6px;">
      <button type="button" class="btn btn-ghost btn-sm" onclick="document.getElementById('tls-file-${part}').click()">Choose file…</button>
      <span id="tls-fname-${part}" style="font-size:12px;color:var(--text-muted);">${current ? '' : 'or paste below'}</span>
      <input type="file" id="tls-file-${part}" style="display:none;" accept=".pem,.crt,.cer,.key,.der,.p12,.pfx" onchange="tlsReadFile('${part}', this)">
    </div>
    <textarea id="tls-${part}" rows="3" spellcheck="false" style="font-family:var(--mono,monospace);font-size:11px;"
      placeholder="${current ? 'Paste or choose a file to replace it' : `-----BEGIN ${begin}-----`}"></textarea>
    <div class="form-hint">${hint}</div>
  </div></div>`;
}

function tlsEditProfile(name) {
  const p = name ? (tlsProfiles || []).find(x => x.name === name) : null;
  tlsEditing = p ? p.name : '';
  const leaf = p && p.cert && p.cert[0];
  const ed = document.getElementById('tls-editor');
  ed.innerHTML = `
    <div style="border-top:1px solid var(--border);margin-top:14px;padding-top:14px;display:flex;flex-direction:column;gap:12px;">
      <h4 style="font-size:13px;margin:0;">${p ? `Edit profile <b>${escHtml(p.name)}</b>` : 'New TLS profile'}</h4>
      <div class="form-row"><div class="form-group" style="max-width:320px;">
        <label>Name</label>
        <input type="text" id="tls-name" value="${p ? escHtml(p.name) : ''}" ${p ? 'disabled' : ''} maxlength="48" placeholder="e.g. upload-server">
        ${p ? '' : '<div class="form-hint">Letters, digits, spaces, - _ and . — shown in the rule editor</div>'}
      </div></div>
      ${tlsPemField('ca', 'Server CA certificate <span style="color:var(--text-muted);font-weight:400;">(optional)</span>',
        "The CA that signed the server's certificate, when the server doesn't use a public CA. When set, only this CA is trusted.",
        p && p.ca ? escHtml(p.ca.map(c => c.cn || cnOf(c.subject)).join(', ')) : '')}
      ${tlsPemField('cert', 'Client certificate <span style="color:var(--text-muted);font-weight:400;">(optional)</span>',
        'Sent to servers that require a client certificate (mutual TLS). PEM; may also contain the chain and the private key.',
        leaf ? `${escHtml(leaf.cn || leaf.subject)}, issued by ${escHtml(cnOf(leaf.issuer))}, valid until ${fmtCertDate(leaf.not_after)}` : '')}
      ${tlsPemField('key', 'Private key',
        'Required with a client certificate. Stored on the camera and never shown again.',
        p && p.key ? `${escHtml(p.key.type)} ${p.key.bits}-bit key` : '')}
      <div class="form-row"><div class="form-group" style="max-width:320px;">
        <label>Key passphrase <span style="color:var(--text-muted);font-weight:400;">(only if the key is encrypted)</span></label>
        <input type="password" id="tls-passphrase" autocomplete="new-password">
      </div></div>
      <div class="form-row"><div class="form-group">
        <label style="display:flex;align-items:center;gap:8px;cursor:pointer;">
          <input type="checkbox" id="tls-insecure" ${p && p.insecure ? 'checked' : ''}>
          Don't verify server certificate
        </label>
        <div class="form-hint">The connection is still encrypted and the client certificate is still sent, but the server's identity isn't checked. Use only for testing or servers with self-signed certificates.</div>
      </div></div>
      <div id="tls-error" style="display:none;font-size:13px;color:var(--accent-danger);"></div>
      <div style="display:flex;gap:8px;justify-content:flex-end;">
        <button class="btn btn-ghost" onclick="tlsCancelEdit()">Cancel</button>
        <button class="btn btn-primary" id="tls-save-btn" onclick="tlsSaveProfile()">Save Profile</button>
      </div>
    </div>`;
  ed.style.display = '';
  ed.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  if (!p) document.getElementById('tls-name').focus();
}

function tlsCancelEdit() {
  tlsEditing = null;
  const ed = document.getElementById('tls-editor');
  ed.innerHTML = '';
  ed.style.display = 'none';
}

function showTlsError(msg) {
  const el = document.getElementById('tls-error');
  if (!el) { toast(msg, 'error'); return; }
  el.textContent = msg;
  el.style.display = msg ? '' : 'none';
}

async function tlsReadFile(part, input) {
  const f = input.files && input.files[0];
  input.value = '';
  if (!f) return;
  if (/\.(p12|pfx)$/i.test(f.name)) {
    showTlsError(`${f.name} is a PKCS#12 bundle, which isn't supported yet. Convert it to PEM first, ` +
                 `for example: openssl pkcs12 -in ${f.name} -out bundle.pem -nodes — then choose bundle.pem as the client certificate.`);
    return;
  }
  const buf = new Uint8Array(await f.arrayBuffer());
  if (buf.length > 48 * 1024) { showTlsError(`${f.name} is larger than 48 KB`); return; }
  let text = new TextDecoder().decode(buf);
  if (!text.includes('-----BEGIN')) {
    /* Binary DER (.cer/.der): wrap it as PEM */
    let bin = '';
    buf.forEach(b => { bin += String.fromCharCode(b); });
    const label = part === 'key' ? 'PRIVATE KEY' : 'CERTIFICATE';
    text = `-----BEGIN ${label}-----\n${btoa(bin).replace(/(.{64})/g, '$1\n')}\n-----END ${label}-----\n`;
  }
  document.getElementById(`tls-${part}`).value = text;
  document.getElementById(`tls-fname-${part}`).textContent = f.name;
  showTlsError('');
}

async function tlsSaveProfile() {
  if (tlsEditing === null) return;
  const isNew = tlsEditing === '';
  const name = isNew ? document.getElementById('tls-name').value.trim() : tlsEditing;
  if (!name) { showTlsError('Enter a name for the profile'); return; }
  if (isNew && (tlsProfiles || []).some(p => p.name === name)) {
    showTlsError(`A profile named "${name}" already exists. Use Edit to change it.`);
    return;
  }
  const body = { name, insecure: document.getElementById('tls-insecure').checked };
  for (const part of ['ca', 'cert', 'key']) {
    const txt = document.getElementById(`tls-${part}`).value.trim();
    const rm = document.getElementById(`tls-remove-${part}`);
    if (txt) body[part] = txt;
    else if (rm && rm.checked) body[part] = '';
  }
  /* A key without its certificate is useless: remove them together */
  if (body.cert === '' && body.key === undefined) body.key = '';
  const pass = document.getElementById('tls-passphrase').value;
  if (pass) body.passphrase = pass;
  if (JSON.stringify(body).length > 60000) { showTlsError('Certificates and key together must be under 60 KB'); return; }

  const btn = document.getElementById('tls-save-btn');
  btn.disabled = true;
  showTlsError('');
  try {
    await API.saveCert(body);
    toast(`TLS profile "${name}" saved`, 'success');
    tlsCancelEdit();
    await loadTlsProfiles();
  } catch(e) {
    showTlsError(apiErrorText(e));
  } finally {
    btn.disabled = false;
  }
}

async function tlsDeleteProfile(name) {
  const p = (tlsProfiles || []).find(x => x.name === name);
  const users = (p && p.used_by) || [];
  if (users.length) {
    toast(`"${name}" is used by ${users.map(u => u.name).join(', ')}. Choose another profile there first.`, 'error');
    return;
  }
  if (!confirm(`Delete TLS profile "${name}"? Its certificates and key are removed from the camera.`)) return;
  try {
    await API.deleteCert(name);
    toast(`TLS profile "${name}" deleted`, 'success');
    if (tlsEditing === name) tlsCancelEdit();
    await loadTlsProfiles();
  } catch(e) {
    toast(apiErrorText(e), 'error');
  }
}

function fillSettingsTlsSelects() {
  fillTlsProfileSelect(document.querySelector('#mqtt-form [name="tls_profile"]'));
  fillTlsProfileSelect(document.getElementById('smtp-tls-profile'));
}

/* Profile picker in the settings forms; the saved choice is kept in data-value */
function fillTlsProfileSelect(sel) {
  if (!sel) return;
  const cur = sel.dataset.value !== undefined ? sel.dataset.value : sel.value;
  const list = Array.isArray(tlsProfiles) ? tlsProfiles : [];
  const missing = cur && !list.some(p => p.name === cur);
  sel.innerHTML = `<option value="">None — public CAs, no client certificate</option>` +
    list.map(p => `<option value="${escHtml(p.name)}">${escHtml(p.name)}${escHtml(tlsProfileLabel(p))}</option>`).join('') +
    (missing ? `<option value="${escHtml(cur)}">${escHtml(cur)} (missing)</option>` : '');
  sel.value = cur || '';
}

function mqttTlsRowVisibility() {
  const form = document.getElementById('mqtt-form');
  const row = document.getElementById('mqtt-tls-profile-row');
  if (form && row) row.style.display = form.querySelector('[name="use_tls"]').checked ? '' : 'none';
}

/* ===== Serial Port / RS-485 Setup ===== */

async function vapixParam(params) {
  const qs = Object.entries(params).map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`).join('&');
  const r = await fetch(`/axis-cgi/param.cgi?${qs}`, { credentials: 'include' });
  if (!r.ok) throw new Error(`param.cgi failed: ${r.status}`);
  return r.text();
}

async function checkSerialPort() {
  const card = document.getElementById('serial-setup-card');
  const dot  = document.getElementById('serial-dot');
  const text = document.getElementById('serial-status-text');
  try {
    const [pmRes, serRes] = await Promise.all([
      vapixParam({ action: 'list', group: 'PortManager' }),
      vapixParam({ action: 'list', group: 'Serial' }).catch(() => '')
    ]);
    if (!pmRes || pmRes.trim() === '' || pmRes.includes('Error')) {
      /* No PortManager -- hide the card */
      card.style.display = 'none';
      return;
    }
    card.style.display = 'block';
    const isRS485   = serRes.includes('PortMode=RS485');
    const isEnabled = pmRes.includes('GenericTCPServer.Enabled=yes') && pmRes.includes('Listener.Enabled=yes');
    _serialModeRS485 = isRS485;
    const isConfigured = isRS485 && isEnabled;
    setFeatureSwitch('serial', isConfigured);
    initSectionOpen('serial', isConfigured);
    if (isRS485 && isEnabled) {
      dot.style.background = 'var(--accent-success)';
      text.textContent = 'Configured';
    } else {
      dot.style.background = '#f59e0b';
      text.textContent = isRS485 ? 'RS-485 set, TCP listener not enabled' : 'Not configured';
    }
    const m = serRes.match(/BaudRate=(\d+)/);
    if (m) {
      const sel = document.getElementById('serial-baud');
      if (![...sel.options].some(opt => opt.value === m[1])) {
        const opt = document.createElement('option');
        opt.value = m[1];
        opt.textContent = m[1];
        sel.appendChild(opt);
      }
      for (const opt of sel.options) { if (opt.value === m[1]) { opt.selected = true; break; } }
    }
    const pm = pmRes.match(/Listener\.Port=(\d+)/);
    if (pm) document.getElementById('serial-port').value = pm[1];
  } catch(e) {
    _serialModeRS485 = false;
    setFeatureSwitch('serial', false);
    /* Not supported or not authenticated -- hide silently */
    card.style.display = 'none';
  }
}

async function configureSerial() {
  const baud = document.getElementById('serial-baud').value;
  const port = parseInt(document.getElementById('serial-port').value, 10);
  const log  = document.getElementById('serial-log');
  const btn  = document.getElementById('serial-configure-btn');

  if (!port || port < 1024 || port > 65535) { toast('Invalid TCP port', 'error'); return false; }

  log.style.display = 'block';
  log.textContent = '';
  btn.disabled = true;
  const emit = (msg) => { log.textContent += msg + '\n'; log.scrollTop = log.scrollHeight; };

  try {
    emit('Setting serial port to RS-485 mode...');
    await vapixParam({
      action: 'update',
      'Serial.Ser1.PortMode': 'RS485',
      'Serial.Ser1.BaudRate': baud,
      'Serial.Ser1.DataBits': '8',
      'Serial.Ser1.Parity':   'None',
      'Serial.Ser1.StopBits': '1'
    });
    emit('OK');

    emit('Enabling PortManager GenericTCPServer listener on port ' + port + '...');
    await vapixParam({
      action: 'update',
      'PortManager.P0.GenericTCPServer.Enabled':          'yes',
      'PortManager.P0.GenericTCPServer.Listener.Enabled': 'yes',
      'PortManager.P0.GenericTCPServer.Listener.Port':    String(port)
    });
    emit('OK');

    emit('Restarting PortManager...');
    await vapixParam({ action: 'update', 'PortManager.P0.PortEnabled': 'no' });
    await new Promise(r => setTimeout(r, 800));
    await vapixParam({ action: 'update', 'PortManager.P0.PortEnabled': 'yes' });
    emit('OK');

    emit('Done. Use connection type "Serial Gateway", host 127.0.0.1, port ' + port + ' in rules.');
    toast('RS-485 configured', 'success');
    await checkSerialPort();
    return true;
  } catch(e) {
    emit('Error: ' + e.message);
    toast('Configuration failed', 'error');
    await checkSerialPort();
    return false;
  } finally {
    btn.disabled = false;
  }
}

async function enableSerialListener() {
  const port = parseInt(document.getElementById('serial-port').value, 10);
  const log = document.getElementById('serial-log');
  const emit = (msg) => { log.textContent += msg + '\n'; log.scrollTop = log.scrollHeight; };
  if (!port || port < 1024 || port > 65535) { toast('Invalid TCP port', 'error'); return false; }

  log.style.display = 'block';
  log.textContent = '';
  try {
    emit('Enabling the existing RS-485 TCP listener on port ' + port + '...');
    await vapixParam({
      action: 'update',
      'PortManager.P0.GenericTCPServer.Enabled':          'yes',
      'PortManager.P0.GenericTCPServer.Listener.Enabled': 'yes',
      'PortManager.P0.GenericTCPServer.Listener.Port':    String(port)
    });
    emit('OK');
    emit('Restarting PortManager...');
    await vapixParam({ action: 'update', 'PortManager.P0.PortEnabled': 'no' });
    await new Promise(r => setTimeout(r, 800));
    await vapixParam({ action: 'update', 'PortManager.P0.PortEnabled': 'yes' });
    emit('OK');
    toast('RS-485 gateway turned on');
    await checkSerialPort();
    return true;
  } catch(e) {
    emit('Error: ' + e.message);
    toast('Failed to turn on: ' + e.message, 'error');
    await checkSerialPort();
    return false;
  }
}

/* Closes the TCP listener and keeps the serial port settings (baud, RS-485
 * mode, port number), so turning it back on restores the same setup. */
async function disableSerial() {
  const log = document.getElementById('serial-log');
  const emit = (msg) => { log.textContent += msg + '\n'; log.scrollTop = log.scrollHeight; };
  log.style.display = 'block';
  log.textContent = '';
  try {
    emit('Disabling PortManager GenericTCPServer listener...');
    await vapixParam({ action: 'update', 'PortManager.P0.GenericTCPServer.Listener.Enabled': 'no' });
    emit('OK');
    emit('Restarting PortManager...');
    await vapixParam({ action: 'update', 'PortManager.P0.PortEnabled': 'no' });
    await new Promise(r => setTimeout(r, 800));
    await vapixParam({ action: 'update', 'PortManager.P0.PortEnabled': 'yes' });
    emit('OK');
    toast('RS-485 gateway turned off');
  } catch(e) {
    emit('Error: ' + e.message);
    toast('Failed to turn off: ' + e.message, 'error');
  }
  await checkSerialPort();
}
