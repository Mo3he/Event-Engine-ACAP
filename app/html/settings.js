'use strict';

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
      <h3 style="font-size:13px;color:var(--text-muted);margin-bottom:12px;">DEVICE</h3>
      <table style="width:100%;font-size:13px;border-collapse:collapse;">
        ${[['Model', dev.model],['Serial', dev.serial],['IP', dev.ip],
           ['Firmware', dev.firmware],['Engine', s.engine_version]].map(([k, v]) => v ? `
          <tr>
            <td style="padding:5px 0;color:var(--text-muted);width:120px;">${k}</td>
            <td style="padding:5px 0;">${escHtml(v)}</td>
          </tr>` : '').join('')}
      </table>`;

    document.getElementById('header-subtitle').textContent =
      `${dev.model || ''} · ${dev.serial || ''} · ${s.rules_enabled}/${s.rules_total} rules active`;

    /* Update MQTT status badge (non-destructive — doesn't touch the form) */
    updateMqttStatusBadge(s.mqtt || {});
    updateSparkplugStatusBadge(s.sparkplug || {});
    loadSparkplugLiveMetrics();
    updateStreamStatusBadge(s);
    renderDeviceCapabilities();
    checkSerialPort();
  } catch(e) {
    toast('Failed to load status', 'error');
  }
}

function renderDeviceCapabilities() {
  const el = document.getElementById('capabilities-card');
  if (!el) return;
  const rows = [];
  const yesNo = (v, yes) => v ? `<span style="color:var(--accent-success);">${yes}</span>` : `<span style="color:var(--text-dim);">No</span>`;

  /* PTZ */
  if (ptzPresets === null) {
    rows.push(['PTZ', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else if (!ptzPresets.length) {
    rows.push(['PTZ', yesNo(false)]);
  } else {
    const total = ptzPresets.reduce((n, c) => n + c.presets.length, 0);
    rows.push(['PTZ', `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${total} preset${total !== 1 ? 's' : ''} on ${ptzPresets.length} channel${ptzPresets.length !== 1 ? 's' : ''})</span>`]);
  }

  /* Audio Clips */
  if (audioClips === null) {
    rows.push(['Audio Clips', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else {
    rows.push(['Audio Clips', audioClips.length
      ? `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${audioClips.length} clip${audioClips.length !== 1 ? 's' : ''})</span>`
      : yesNo(false)]);
  }

  /* Siren & Light */
  if (sirenProfiles === null) {
    rows.push(['Siren & Light', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else {
    rows.push(['Siren & Light', sirenProfiles.length
      ? `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${sirenProfiles.length} profile${sirenProfiles.length !== 1 ? 's' : ''})</span>`
      : yesNo(false)]);
  }

  /* Privacy Masks */
  if (privacyMasks !== null && privacyMasks.length) {
    rows.push(['Privacy Masks', `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${privacyMasks.length})</span>`]);
  }

  /* Guard Tours */
  if (guardTours !== null && guardTours.length) {
    rows.push(['Guard Tours', `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${guardTours.length})</span>`]);
  }

  /* Object Analytics */
  if (aoaScenarios === null) {
    rows.push(['Object Analytics', '<span style="color:var(--text-dim);">Loading…</span>']);
  } else {
    rows.push(['Object Analytics (AOA)', aoaScenarios.length
      ? `<span style="color:var(--accent-success);">Yes</span> <span style="color:var(--text-dim);">(${aoaScenarios.length} scenario${aoaScenarios.length !== 1 ? 's' : ''})</span>`
      : yesNo(false)]);
  }

  /* Installed ACAPs */
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
 * MQTT tab
 * =================================================== */
function updateMqttStatusBadge(mq) {
  const dot  = document.getElementById('mqtt-dot');
  const text = document.getElementById('mqtt-status-text');
  if (!dot || !text) return;
  const suffix = mq.use_tls ? ' · TLS' : '';
  if (mq.connected) {
    dot.style.background = 'var(--accent-success)';
    text.textContent = `Connected — ${escHtml(mq.host || '')}:${mq.port || 1883}${suffix}`;
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
  if (clients > 0) {
    dot.style.background = 'var(--accent-success)';
    text.textContent = `${clients} client${clients !== 1 ? 's' : ''} connected`;
  } else {
    dot.style.background = 'var(--text-dim, #555)';
    text.textContent = 'No clients';
  }
  /* Fill in the actual camera IP in the URL display */
  if (urlEl && status.device && status.device.ip) {
    urlEl.textContent = `http://${status.device.ip}/local/acap_event_engine/alertStream`;
  }
}

/*------------------------------------------------------------
 * Solar event calculator (mirrors scheduler.c algorithm)
 * Returns "HH:MM" local time string, or null (polar day/night).
 *------------------------------------------------------------*/
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
    const r = await API.post('settings', { engine: { socks5_proxy: proxy } });
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
    const r = await API.post('settings', { engine: { latitude: lat, longitude: lon } });
    engineLat = lat;
    engineLon = lon;
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
  };
  const pw = el('smtp-pass') ? el('smtp-pass').value : '';
  if (pw) data.password = pw;
  try {
    const r = await API.post('settings', { smtp: data });
    toast('SMTP settings saved');
    if (el('smtp-pass')) el('smtp-pass').value = '';
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
    form.querySelector('[name="enabled"]').checked = !!mq.enabled;
    form.querySelector('[name="servers"]').value = (mq.servers || []).join('\n');
    /* Don't pre-fill password — leave placeholder "(unchanged)" */

    /* Also refresh the status badge */
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
      enabled:   form.querySelector('[name="enabled"]').checked,
      host:      form.querySelector('[name="host"]').value.trim(),
      port:      parseInt(form.querySelector('[name="port"]').value, 10) || 1883,
      client_id: form.querySelector('[name="client_id"]').value.trim(),
      username:  form.querySelector('[name="username"]').value.trim(),
      keepalive: parseInt(form.querySelector('[name="keepalive"]').value, 10) || 60,
      use_tls:   form.querySelector('[name="use_tls"]').checked,
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

/* The SOAP catalog only carries a namespace prefix on the levels where the XML
 * document declares one ("tnsaxis:CameraApplicationPlatform" but a bare
 * "acap_event_engine" below it), while saved config qualifies every level.
 * Compare local names only — which is also what the event system matches on. */
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

/* A binding may reference an event the SOAP catalog does not list (it omits
 * CameraApplicationPlatform topics entirely), so offer both a "keep" option and
 * a way to type a topic path directly. */
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

    form.querySelector('[name="enabled"]').checked       = !!sp.enabled;
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
  const enabled = form.querySelector('[name="enabled"]').checked;
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
    loadEngineSettings(settings);
    loadProxySettings(settings);
    loadSmtpSettings(settings);
    loadMqttSettings(settings);
    loadSparkplugSettings(settings);
  } catch(e) { /* non-fatal — individual loaders will show errors as needed */ }
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
    if (isRS485 && isEnabled) {
      dot.style.background = 'var(--accent-success)';
      text.textContent = 'Configured';
    } else {
      dot.style.background = '#f59e0b';
      text.textContent = isRS485 ? 'RS-485 set, TCP listener not enabled' : 'Not configured';
    }
    /* Pre-fill baud if already set */
    const m = serRes.match(/BaudRate=(\d+)/);
    if (m) {
      const sel = document.getElementById('serial-baud');
      for (const opt of sel.options) { if (opt.value === m[1]) { opt.selected = true; break; } }
    }
    const pm = pmRes.match(/Listener\.Port=(\d+)/);
    if (pm) document.getElementById('serial-port').value = pm[1];
  } catch(e) {
    /* Not supported or not authenticated -- hide silently */
    card.style.display = 'none';
  }
}

async function configureSerial() {
  const baud = document.getElementById('serial-baud').value;
  const port = parseInt(document.getElementById('serial-port').value, 10);
  const log  = document.getElementById('serial-log');
  const btn  = event.currentTarget;

  if (!port || port < 1024 || port > 65535) { toast('Invalid TCP port', 'error'); return; }

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
  } catch(e) {
    emit('Error: ' + e.message);
    toast('Configuration failed', 'error');
  } finally {
    btn.disabled = false;
  }
}
