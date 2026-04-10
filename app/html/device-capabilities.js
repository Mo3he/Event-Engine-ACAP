'use strict';

/* ------------------------------------------------------------
 * Remote capability cache: key = "host:::query", value = array
 * ------------------------------------------------------------ */
const remoteCapCache = {};

/* Fetch capabilities from a remote Axis device via the /remote-caps proxy endpoint.
 * query: "ptz" | "audio" | "siren" | "privacy" | "guardtour" | "acap"
 * Returns an array of {value, label} objects, or null on failure. */
async function fetchRemoteCaps(query, host, user, pass) {
  const key = `${host}:::${query}`;
  if (remoteCapCache[key]) return remoteCapCache[key];
  try {
    const resp = await fetch('/local/acap_event_engine/remote-caps', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ host, user: user || 'root', pass: pass || '', query })
    });
    if (!resp.ok) return null;
    const data = await resp.json();
    if (!Array.isArray(data)) return null;
    remoteCapCache[key] = data;
    return data;
  } catch(e) { return null; }
}

/* Called by "Load from device" buttons in action rows.
 * Fetches capabilities, caches them, and re-renders the action list. */
async function loadRemoteCap(rowIndexStr, query) {
  const rowIndex = parseInt(rowIndexStr);
  const row = document.getElementById('arow-' + rowIndex);
  if (!row) return;
  const hostEl = row.querySelector('[data-k="remote_host"]');
  const userEl = row.querySelector('[data-k="remote_user"]');
  const passEl = row.querySelector('[data-k="remote_pass"]');
  if (!hostEl || !hostEl.value.trim()) { toast('Enter remote device IP first', 'warning'); return; }
  const btn = row.querySelector('.remote-load-btn[data-q="' + query + '"]');
  if (btn) { btn.disabled = true; btn.textContent = '…'; }
  const result = await fetchRemoteCaps(
    query, hostEl.value.trim(),
    userEl ? userEl.value.trim() : 'root',
    passEl ? passEl.value : '');
  if (btn) { btn.disabled = false; btn.textContent = 'Load'; }
  if (!result) { toast('Could not fetch capabilities from remote device', 'error'); return; }
  /* Preserve current form values then re-render */
  const data = { type: actionRows[rowIndex].type };
  row.querySelectorAll('[data-k]').forEach(inp => {
    data[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
  });
  actionRows[rowIndex] = data;
  renderActionList();
}

/* Called by "Load" buttons in condition rows (aoa_occupancy and vapix_event_state).
 * Works identically to loadRemoteCap but targets crow-{rowIndex} and conditionRows. */
async function loadRemoteCondCap(rowIndexStr, query) {
  const rowIndex = parseInt(rowIndexStr);
  const row = document.getElementById('crow-' + rowIndex);
  if (!row) return;
  const hostEl = row.querySelector('[data-k="remote_host"]');
  const userEl = row.querySelector('[data-k="remote_user"]');
  const passEl = row.querySelector('[data-k="remote_pass"]');
  if (!hostEl || !hostEl.value.trim()) { toast('Enter remote device IP first', 'warning'); return; }
  const host = hostEl.value.trim();
  const btn = row.querySelector('.remote-load-btn[data-q="' + query + '"]');
  if (btn) { btn.disabled = true; btn.textContent = '…'; }
  const result = await fetchRemoteCaps(
    query, host,
    userEl ? userEl.value.trim() : 'root',
    passEl ? passEl.value : '');
  if (btn) { btn.disabled = false; btn.textContent = 'Load'; }
  if (!result) { toast('Could not fetch options from remote device', 'error'); return; }

  /* vapix_events returns [{soap: "<raw xml>"}] — parse with existing catalog parser */
  if (query === 'vapix_events' && result[0] && result[0].soap) {
    const parsed = parseVapixEventCatalog(result[0].soap);
    remoteCapCache[`${host}:::vapix_events_catalog`] = parsed;  /* full catalog — shared with action loader */
    remoteCapCache[`${host}:::vapix_events`] = parsed
      .map(ev => ({ topic: vapixCatalogTopicPath(ev), dataKeys: ev.dataKeys }))
      .filter(ev => ev.topic);
    if (!remoteCapCache[`${host}:::vapix_events`].length)
      toast('No events found on remote device', 'warning');
  }

  collectConditionRow(rowIndex);
  renderConditionList();
}

/* Called by "Load Events" button in a remote vapix_query action row.
 * Reuses the full catalog cached by the condition loader when available;
 * otherwise fetches directly (not via fetchRemoteCaps, whose cache key
 * gets overwritten by loadRemoteCondCap with condition-formatted data). */
async function loadRemoteActionVapixEvents(rowIndexStr) {
  const rowIndex = parseInt(rowIndexStr);
  const row = document.getElementById('arow-' + rowIndex);
  if (!row) return;
  const hostEl = row.querySelector('[data-k="remote_host"]');
  const userEl = row.querySelector('[data-k="remote_user"]');
  const passEl = row.querySelector('[data-k="remote_pass"]');
  if (!hostEl || !hostEl.value.trim()) { toast('Enter remote device IP first', 'warning'); return; }
  const host = hostEl.value.trim();
  const cacheKey = `${host}:::vapix_events_catalog`;

  /* If already cached (e.g. condition side loaded it), skip the fetch */
  let parsed = remoteCapCache[cacheKey];
  if (!parsed) {
    const btn = row.querySelector('.btn-ghost[onclick*="loadRemoteActionVapixEvents"]');
    if (btn) { btn.disabled = true; btn.textContent = '…'; }
    try {
      const resp = await fetch('/local/acap_event_engine/remote-caps', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          host,
          user: (userEl ? userEl.value.trim() : '') || 'root',
          pass: passEl ? passEl.value : '',
          query: 'vapix_events'
        })
      });
      if (btn) { btn.disabled = false; btn.textContent = 'Load Events'; }
      if (!resp.ok) { toast('Could not fetch event catalog from remote device', 'error'); return; }
      const raw = await resp.json();
      if (!Array.isArray(raw) || !raw[0] || !raw[0].soap) {
        toast('Could not fetch event catalog from remote device', 'error'); return;
      }
      parsed = parseVapixEventCatalog(raw[0].soap);
      remoteCapCache[cacheKey] = parsed;
    } catch(e) {
      if (btn) { btn.disabled = false; btn.textContent = 'Load Events'; }
      toast('Could not fetch event catalog from remote device', 'error'); return;
    }
  }
  if (!parsed.length) { toast('No events found on remote device', 'warning'); return; }
  /* Preserve current form values then re-render */
  const data = { type: actionRows[rowIndex].type };
  row.querySelectorAll('[data-k]').forEach(inp => {
    data[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
  });
  actionRows[rowIndex] = data;
  renderActionList();
}

/* Called from the remote vapix_query action dropdown — updates the hidden topic inputs
 * using the remote event catalog instead of the local vapixEventCatalog. */
function applyRemoteVapixEventAction(sel, host) {
  const idx = parseInt(sel.value);
  const cacheKey = `${host}:::vapix_events_catalog`;
  const catalog = (typeof remoteCapCache !== 'undefined') ? remoteCapCache[cacheKey] : null;
  const ev = catalog && catalog[idx];
  if (!ev) return;
  const row = sel.closest('.tca-row');
  ['topic0','topic1','topic2','topic3'].forEach(k => {
    const nsEl  = row.querySelector(`[data-k="${k}_ns"]`);
    const valEl = row.querySelector(`[data-k="${k}_val"]`);
    if (!nsEl || !valEl) return;
    if (ev.topics[k]) {
      nsEl.value  = Object.keys(ev.topics[k])[0]   || '';
      valEl.value = Object.values(ev.topics[k])[0] || '';
    } else {
      nsEl.value = ''; valEl.value = '';
    }
  });
}

async function loadVapixEventCatalog() {
  try {
    const resp = await fetch('/vapix/services', {
      method: 'POST',
      headers: { 'Content-Type': 'application/soap+xml; charset=utf-8' },
      body: `<?xml version="1.0" encoding="UTF-8"?><soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope" xmlns:tev="http://www.onvif.org/ver10/events/wsdl"><soap:Body><tev:GetEventProperties/></soap:Body></soap:Envelope>`
    });
    if (!resp.ok) throw new Error(resp.status);
    vapixEventCatalog = parseVapixEventCatalog(await resp.text());
  } catch(e) {
    vapixEventCatalog = [];
  }
  /* If the rule editor is open, re-render so catalog-based hints and dropdowns update */
  if (!document.getElementById('modal-overlay').classList.contains('hidden')) {
    renderTriggerList();
    renderActionList();
  }
}

async function loadPtzPresets() {
  try {
    const resp = await fetch('/axis-cgi/com/ptz.cgi?query=presetposall');
    if (!resp.ok) { ptzPresets = []; renderDeviceCapabilities(); return; }
    const text = await resp.text();
    const channels = [];
    let cur = null;
    for (const line of text.split('\n')) {
      const m = line.match(/^Preset Positions for camera (\d+)/);
      if (m) { cur = { channel: parseInt(m[1]), presets: [] }; channels.push(cur); }
      else if (cur) {
        const pm = line.match(/^presetposno\d+=(.+)/);
        if (pm) cur.presets.push(pm[1].trim());
      }
    }
    ptzPresets = channels.filter(c => c.presets.length);
  } catch(e) { ptzPresets = []; }
  renderDeviceCapabilities();
}

async function loadAudioClips() {
  try {
    const resp = await fetch('/axis-cgi/param.cgi?action=list&group=MediaClip');
    if (!resp.ok) { audioClips = []; return; }
    const text = await resp.text();
    const byId = {};
    for (const line of text.split('\n')) {
      const m = line.match(/^root\.MediaClip\.(M\d+)\.(\w+)=(.+)/);
      if (!m) continue;
      const [, key, prop, val] = m;
      if (!byId[key]) byId[key] = { id: key.slice(1) };
      if (prop === 'Name') byId[key].name = val.trim();
      if (prop === 'Type') byId[key].type = val.trim();
    }
    audioClips = Object.values(byId)
      .filter(c => c.type === 'audio' && c.name)
      .sort((a, b) => parseInt(a.id) - parseInt(b.id));
  } catch(e) { audioClips = []; }
  renderDeviceCapabilities();
}

async function loadSirenProfiles() {
  try {
    const resp = await fetch('/axis-cgi/siren_and_light.cgi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ apiVersion: '1.0', method: 'getProfiles' })
    });
    if (!resp.ok) { sirenProfiles = []; return; }
    const data = await resp.json();
    const profiles = data && data.data && data.data.profiles;
    if (!Array.isArray(profiles)) { sirenProfiles = []; return; }
    sirenProfiles = profiles.map(p => ({ name: p.name, label: p.name }));
  } catch(e) { sirenProfiles = []; }
  renderDeviceCapabilities();
}

async function loadAcapApps() {
  try {
    const resp = await fetch('/axis-cgi/applications/list.cgi');
    if (!resp.ok) { acapApps = []; return; }
    const text = await resp.text();
    const parser = new DOMParser();
    const doc = parser.parseFromString(text, 'application/xml');
    const apps = [];
    doc.querySelectorAll('application').forEach(el => {
      const pkg      = el.getAttribute('Name');
      const niceName = el.getAttribute('NiceName') || pkg;
      if (pkg) apps.push({ package: pkg, niceName });
    });
    acapApps = apps.sort((a, b) => a.niceName.localeCompare(b.niceName));
  } catch(e) { acapApps = []; }
  renderDeviceCapabilities();
  if (!document.getElementById('modal-overlay').classList.contains('hidden'))
    renderActionList();
}

async function loadPrivacyMasks() {
  try {
    const resp = await fetch('/axis-cgi/privacymask.cgi?query=listpxjson');
    if (!resp.ok) { privacyMasks = []; return; }
    const data = await resp.json();
    const list = data && data.listpx;
    if (!Array.isArray(list)) { privacyMasks = []; return; }
    privacyMasks = list.map(m => ({ name: m.name }));
  } catch(e) { privacyMasks = []; }
  renderDeviceCapabilities();
  if (!document.getElementById('modal-overlay').classList.contains('hidden'))
    renderActionList();
}

async function loadDeviceParams() {
  try {
    const resp = await fetch('/axis-cgi/param.cgi?action=list');
    if (!resp.ok) { deviceParams = []; return; }
    const text = await resp.text();
    const params = [];
    for (const line of text.split('\n')) {
      const eq = line.indexOf('=');
      if (eq > 0) params.push(line.slice(0, eq).trim());
    }
    deviceParams = params;
  } catch(e) { deviceParams = []; }
  if (!document.getElementById('modal-overlay').classList.contains('hidden'))
    renderActionList();
}

async function loadGuardTours() {
  try {
    const resp = await fetch('/axis-cgi/param.cgi?action=list&group=GuardTour');
    if (!resp.ok) { guardTours = []; return; }
    const text = await resp.text();
    const byId = {};
    for (const line of text.split('\n')) {
      const m = line.match(/^root\.GuardTour\.G(\d+)\.Name=(.+)/);
      if (!m) continue;
      const id = parseInt(m[1]);
      const name = m[2].trim().replace(/\r$/, '');
      if (name) byId[id] = { id, name };
    }
    guardTours = Object.values(byId).sort((a, b) => a.id - b.id);
  } catch(e) { guardTours = []; }
  renderDeviceCapabilities();
  if (!document.getElementById('modal-overlay').classList.contains('hidden'))
    renderActionList();
}

async function loadAoaScenarios() {
  try {
    const resp = await fetch(`${BASE}/aoa`);
    if (!resp.ok) { aoaScenarios = []; return; }
    const data = await resp.json();
    aoaScenarios = Array.isArray(data) ? data : [];
  } catch(e) { aoaScenarios = []; }
  renderDeviceCapabilities();
  if (!document.getElementById('modal-overlay').classList.contains('hidden')) {
    renderTriggerList();
    renderActionList();
    renderConditionList();
  }
}

/* Fetch current value + allowed values for a device parameter.
 * Called onblur from the parameter input in the set_device_param action row.
 * In remote mode, proxies through /remote-caps with query=param. */
async function fetchParamValues(paramInput) {
  const row = paramInput.closest('.tca-row');
  if (!row) return;
  const raw = paramInput.value.trim();
  if (!raw) return;
  const param = raw.startsWith('root.') ? raw : 'root.' + raw;

  const valueInput  = row.querySelector('[data-k="value"]');
  const valListEl   = row.querySelector('.param-val-list');
  const hintEl      = row.querySelector('.param-val-hint');
  if (!valueInput || !hintEl) return;

  /* Detect remote mode */
  const hostEl    = row.querySelector('[data-k="remote_host"]');
  const userEl    = row.querySelector('[data-k="remote_user"]');
  const passEl    = row.querySelector('[data-k="remote_pass"]');
  const toggleEl  = row.querySelector('[data-k="remote_host_toggle"]');
  const isRemote  = toggleEl && toggleEl.value === 'remote' && hostEl && hostEl.value.trim();

  hintEl.textContent = 'Looking up…';

  let currentVal = '', allowedValues = [], allowedLabels = [], paramType = '', defVal = '', minVal = '', maxVal = '';

  if (isRemote) {
    /* Proxy through /remote-caps */
    try {
      const resp = await fetch('/local/acap_event_engine/remote-caps', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          host: hostEl.value.trim(),
          user: userEl ? userEl.value.trim() : 'root',
          pass: passEl ? passEl.value : '',
          query: 'param',
          param_name: param
        })
      });
      if (resp.ok) {
        const data = await resp.json();
        if (Array.isArray(data) && data[0]) {
          const item = data[0];
          currentVal = item.currentValue || '';
          defVal     = item.defaultValue || '';
          /* enumValues is an array of {value, label} objects */
          if (Array.isArray(item.enumValues) && item.enumValues.length) {
            allowedValues = item.enumValues.map(e => e.value);
            allowedLabels = item.enumValues.map(e => e.label || e.value);
          }
        }
      }
    } catch(e) {}
  } else {
    /* Local device */
    try {
      const r = await fetch(`/axis-cgi/param.cgi?action=list&group=${encodeURIComponent(param)}`);
      if (r.ok) {
        const text = await r.text();
        const m = text.match(/=(.+)/);
        if (m) currentVal = m[1].trim().replace(/\r$/, '');
      }
    } catch(e) {}

    try {
      const r = await fetch(`/axis-cgi/param.cgi?action=listdefinitions&group=${encodeURIComponent(param)}&type=all`);
      if (r.ok) {
        const text = await r.text();
        for (const line of text.split('\n')) {
          const eq = line.indexOf('=');
          if (eq < 0) continue;
          const key = line.slice(0, eq).trim();
          const val = line.slice(eq + 1).trim().replace(/\r$/, '');
          if (key.endsWith('.values')) { allowedValues = val.split(',').map(v => v.trim()).filter(Boolean); allowedLabels = [...allowedValues]; }
          else if (key.endsWith('.type')) paramType = val;
          else if (key.endsWith('.def'))  defVal     = val;
          else if (key.endsWith('.min'))  minVal     = val;
          else if (key.endsWith('.max'))  maxVal     = val;
        }
      }
    } catch(e) {}
  }

  /* Update the value input's datalist */
  if (valListEl && allowedValues.length) {
    valListEl.innerHTML = allowedValues.map(v => `<option value="${escHtml(v)}">`).join('');
  }

  /* Build hint line */
  const parts = [];
  if (currentVal) parts.push(`Current: <strong>${escHtml(currentVal)}</strong>`);
  if (paramType)  parts.push(`Type: ${escHtml(paramType)}`);
  if (allowedValues.length)    parts.push(`Values: ${allowedValues.map(v => `<code>${escHtml(v)}</code>`).join(', ')}`);
  else if (minVal || maxVal)   parts.push(`Range: ${escHtml(minVal)}–${escHtml(maxVal)}`);
  if (defVal) parts.push(`Default: ${escHtml(defVal)}`);

  hintEl.innerHTML = parts.length ? parts.join(' &nbsp;·&nbsp; ') : 'No definition found.';

  /* Swap the value text input → <select> when enum values are known */
  if (allowedValues.length) {
    const curVal = valueInput.value || currentVal;
    const sel = document.createElement('select');
    sel.dataset.k = 'value';
    let opts = '<option value="">\u2014 select \u2014</option>';
    for (let j = 0; j < allowedValues.length; j++) {
      const v = allowedValues[j];
      const l = (allowedLabels && allowedLabels[j]) || v;
      opts += `<option value="${escHtml(v)}" ${curVal === v ? 'selected' : ''}>${escHtml(l)}</option>`;
    }
    sel.innerHTML = opts;
    valueInput.replaceWith(sel);
  }
}

function parseVapixEventCatalog(xmlText) {
  try {
    const doc = new DOMParser().parseFromString(xmlText, 'application/xml');
    const WSNT   = 'http://docs.oasis-open.org/wsn/t-1';
    const SCHEMA = 'http://www.onvif.org/ver10/schema';
    const topicSetEl = doc.getElementsByTagNameNS(WSNT, 'TopicSet')[0];
    if (!topicSetEl) return [];
    const events = [];
    function traverse(el, path) {
      for (const child of el.children) {
        const ns = child.namespaceURI || '';
        if (ns.includes('oasis-open.org/wsrf') || ns.includes('oasis-open.org/wsn') ||
            ns.includes('onvif.org/ver10/schema') || ns.includes('onvif.org/ver10/events')) continue;
        const isTopic = child.getAttributeNS(WSNT, 'topic') === 'true';
        const newPath = [...path, { ns: child.prefix || '', name: child.localName }];
        if (isTopic) {
          /* Collect both Source and Data keys — both appear in trigger_data at runtime */
          const getKeys = tag => {
            const el = child.getElementsByTagNameNS(SCHEMA, tag)[0];
            return el
              ? Array.from(el.getElementsByTagNameNS(SCHEMA, 'SimpleItemDescription'))
                  .map(d => d.getAttribute('Name')).filter(Boolean)
              : [];
          };
          const getTypes = tag => {
            const el = child.getElementsByTagNameNS(SCHEMA, tag)[0];
            if (!el) return {};
            const map = {};
            for (const d of el.getElementsByTagNameNS(SCHEMA, 'SimpleItemDescription')) {
              const name = d.getAttribute('Name');
              const raw  = d.getAttribute('Type') || '';
              if (!name) continue;
              if (raw.includes('boolean'))                          map[name] = 'boolean';
              else if (raw.includes('int') || raw.includes('float') ||
                       raw.includes('double') || raw.includes('decimal')) map[name] = 'numeric';
              else                                                  map[name] = 'string';
            }
            return map;
          };
          const dataKeys  = [...getKeys('Source'),  ...getKeys('Data')];
          const dataTypes = { ...getTypes('Source'), ...getTypes('Data') };
          const topics = {};
          newPath.slice(0, 4).forEach((p, i) => { topics[`topic${i}`] = { [p.ns]: p.name }; });
          events.push({ label: newPath.map(p => p.name).join(' / '), topics, dataKeys, dataTypes });
        }
        traverse(child, newPath);
      }
    }
    traverse(topicSetEl, []);
    return events.sort((a, b) => a.label.localeCompare(b.label));
  } catch(e) {
    return [];
  }
}

function findCatalogMatch(t) {
  if (!vapixEventCatalog || !vapixEventCatalog.length) return -1;
  const keys = ['topic0','topic1','topic2','topic3'];
  /* Topics may be stored as {topicN: {ns: val}} (object form from saved JSON /
   * applyVapixEvent) or as {topicN_ns, topicN_val} flat keys (after
   * collectTriggerRow re-reads form inputs).  Normalise to object form. */
  const getTopic = k => {
    if (t[k]) return t[k];
    if (t[`${k}_val`]) return { [t[`${k}_ns`] || '']: t[`${k}_val`] };
    return null;
  };
  const cmp = (a, b) => {
    if (!a && !b) return true;
    if (!a || !b) return false;
    const ak = Object.keys(a)[0], bk = Object.keys(b)[0];
    return ak === bk && a[ak] === b[bk];
  };
  /* Exact match first */
  let idx = vapixEventCatalog.findIndex(ev => keys.every(k => cmp(ev.topics[k], getTopic(k))));
  if (idx >= 0) return idx;
  /* Prefix match: trigger has fewer topic levels than catalog entry */
  return vapixEventCatalog.findIndex(ev =>
    keys.every(k => !getTopic(k) || cmp(ev.topics[k], getTopic(k)))
  );
}

function applyVapixEventAction(sel) {
  /* Called from the vapix_query action dropdown — updates the hidden topic inputs */
  const idx = parseInt(sel.value);
  const ev = vapixEventCatalog && vapixEventCatalog[idx];
  if (!ev) return;
  const row = sel.closest('.tca-row');
  ['topic0','topic1','topic2','topic3'].forEach(k => {
    const nsEl  = row.querySelector(`[data-k="${k}_ns"]`);
    const valEl = row.querySelector(`[data-k="${k}_val"]`);
    if (!nsEl || !valEl) return;
    if (ev.topics[k]) {
      nsEl.value  = Object.keys(ev.topics[k])[0]   || '';
      valEl.value = Object.values(ev.topics[k])[0] || '';
    } else {
      nsEl.value = ''; valEl.value = '';
    }
  });
}

function applyVapixEvent(rowIdx, idx) {
  const ev = vapixEventCatalog && vapixEventCatalog[parseInt(idx)];
  if (!ev) return;
  const row = { type: 'vapix_event', ...ev.topics, _dataTypes: ev.dataTypes || {} };
  triggerRows[rowIdx] = row;
  renderTriggerList();
}

function applyPtzPreset(sel) {
  const row = sel.closest('.tca-row');
  const [ch, ...rest] = sel.value.split(':');
  const name = rest.join(':');
  row.querySelector('[data-k="preset"]').value  = name;
  row.querySelector('[data-k="channel"]').value = ch;
}

function updateWebhookUrl(inp) {
  const urlEl = inp.closest('.tca-row').querySelector('.webhook-url-display');
  if (urlEl) {
    const base = `${window.location.protocol}//${window.location.hostname}/local/acap_event_engine/fire`;
    urlEl.value = `${base}?token=${inp.value}`;
  }
}

/* ===================================================
 * Rule Templates
 * =================================================== */
