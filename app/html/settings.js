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
    renderDeviceCapabilities();
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

function refreshAstroTriggerPreview(rowIdx) {
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

async function loadEngineSettings() {
  try {
    const settings = await API.get('settings');
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

async function loadProxySettings() {
  try {
    const settings = await API.get('settings');
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
    if (!r.ok) throw new Error(await r.text());
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
    if (!r.ok) throw new Error(await r.text());
    engineLat = lat;
    engineLon = lon;
    toast('Engine settings saved');
    refreshSolarPreview(lat, lon, 'engine-solar-preview');
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}

async function loadSmtpSettings() {
  try {
    const settings = await API.get('settings');
    const smtp = (settings && settings.smtp) || {};
    const el = (id) => document.getElementById(id);
    if (el('smtp-server')) el('smtp-server').value = smtp.server || '';
    if (el('smtp-user'))   el('smtp-user').value   = smtp.username || '';
    if (el('smtp-pass'))   el('smtp-pass').value   = '';  /* never show stored pw */
    if (el('smtp-from'))   el('smtp-from').value   = smtp.from || '';
    if (el('smtp-tls'))    el('smtp-tls').value    = smtp.use_tls === false ? 'false' : 'true';
  } catch(e) { /* non-fatal */ }
}

async function saveSmtpSettings(event) {
  event.preventDefault();
  const el = (id) => document.getElementById(id);
  const data = {
    server:   el('smtp-server') ? el('smtp-server').value.trim() : '',
    username: el('smtp-user')   ? el('smtp-user').value.trim()   : '',
    from:     el('smtp-from')   ? el('smtp-from').value.trim()   : '',
    use_tls:  el('smtp-tls')    ? el('smtp-tls').value === 'true': true,
  };
  const pw = el('smtp-pass') ? el('smtp-pass').value : '';
  if (pw) data.password = pw;
  try {
    const r = await API.post('settings', { smtp: data });
    if (!r.ok) throw new Error(await r.text());
    toast('SMTP settings saved');
    if (el('smtp-pass')) el('smtp-pass').value = '';
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}

async function loadMqttSettings() {
  try {
    const settings = await API.get('settings');
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
      use_tls:   form.querySelector('[name="use_tls"]').checked
    }
  };
  /* Only include password if user typed something */
  if (pw) payload.mqtt.password = pw;
  try {
    const r = await API.post('settings', payload);
    if (!r.ok) throw new Error(await r.text());
    toast('MQTT settings saved');
    form.querySelector('[name="password"]').value = '';
    /* Refresh status badge after a short delay to let the engine reconnect */
    setTimeout(loadMqttSettings, 1500);
  } catch(e) {
    toast('Failed to save: ' + e.message, 'error');
  }
}
