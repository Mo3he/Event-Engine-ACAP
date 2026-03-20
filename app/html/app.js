'use strict';

/* ===================================================
 * API Layer
 * =================================================== */
const BASE = '../acap_event_engine';

const API = {
  get:  path => fetch(`${BASE}/${path}`).then(r => r.json()),
  post: (path, body) => fetch(`${BASE}/${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  }),
  put: (path, body) => fetch(`${BASE}/${path}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  }),
  delete: path => fetch(`${BASE}/${path}`, { method: 'DELETE' }),

  getRules:      ()      => API.get('rules'),
  getRule:       id      => API.get(`rules?id=${id}`),
  addRule:       rule    => API.post('rules', rule),
  updateRule:    (id, r) => API.post(`rules?id=${id}`, r),
  deleteRule:    id      => API.delete(`rules?id=${id}`),
  setEnabled:    (id, v) => API.post(`rules?id=${id}`, { enabled: v }),
  fireRule:      id      => API.post('fire', { id }),

  getEvents:     (limit, rule) => API.get(`events?limit=${limit || 100}${rule ? '&rule=' + rule : ''}`),
  getStatus:     ()      => API.get('engine'),
  getVariables:  ()      => API.get('variables'),
  setVariable:   (name, value, is_counter) => API.post('variables', { name, value, is_counter: !!is_counter }),
  deleteVariable: name   => API.delete(`variables?name=${name}`),
};

/* ===================================================
 * State
 * =================================================== */
let allRules = [];
let editingRule = null;       // null = new, object = editing
let pollTimer = null;
let triggerRows = [];
let conditionRows = [];
let actionRows = [];
let vapixEventCatalog = null; /* null = not yet fetched */
let ptzPresets = null;        /* null=loading, []=no PTZ, [{channel,presets:[name,...]},...] */
let audioClips = null;        /* null=loading, []=none,   [{id,name},...] */
let knownVarNames = [];       /* variable names loaded at startup for hint display */
let knownCounterNames = [];   /* counter names loaded at startup for hint display */

/* ===================================================
 * Toast
 * =================================================== */
function toast(msg, type = 'info') {
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  el.textContent = msg;
  document.getElementById('toast-container').appendChild(el);
  setTimeout(() => el.remove(), 3500);
}

/* ===================================================
 * Tabs
 * =================================================== */
document.querySelectorAll('.tab-btn[data-tab]').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
    if (btn.dataset.tab === 'log')       loadEvents();
    if (btn.dataset.tab === 'variables') loadVariables();
    if (btn.dataset.tab === 'status')    loadStatus();
    if (btn.dataset.tab === 'mqtt')      loadMqttSettings();
  });
});

/* ===================================================
 * Utils
 * =================================================== */
function fmtTime(ts) {
  if (!ts) return '—';
  const d = new Date(ts * 1000);
  return d.toLocaleTimeString([], { hour12: false }) + ' ' +
         d.toLocaleDateString([], { month: 'short', day: 'numeric' });
}

function escHtml(s) {
  return String(s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;')
    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

/* ===================================================
 * VAPIX Event Catalog — fetched from the camera at startup
 * =================================================== */
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
    if (!resp.ok) { ptzPresets = []; return; }
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
          const dataKeys = [...getKeys('Source'), ...getKeys('Data')];
          const topics = {};
          newPath.slice(0, 4).forEach((p, i) => { topics[`topic${i}`] = { [p.ns]: p.name }; });
          events.push({ label: newPath.map(p => p.name).join(' / '), topics, dataKeys });
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
  const row = { type: 'vapix_event', ...ev.topics };
  if (ev.dataKeys.length === 1) row.filter_key = ev.dataKeys[0];
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
 * Rules Tab
 * =================================================== */
async function loadRules() {
  try {
    allRules = await API.getRules();
    renderRules();
    updateLogFilter();
  } catch(e) {
    toast('Failed to load rules', 'error');
  }
}

function renderRules() {
  const list = document.getElementById('rules-list');
  const count = document.getElementById('rule-count');
  const enabled = allRules.filter(r => r.enabled).length;
  count.textContent = `(${enabled} / ${allRules.length} enabled)`;

  if (!allRules.length) {
    list.innerHTML = `<div class="empty-state">
      <div class="empty-icon">⚡</div>
      <h3>No rules yet</h3>
      <p>Click <strong>+ New Rule</strong> to create your first rule.</p>
    </div>`;
    return;
  }

  list.innerHTML = allRules.map(r => `
    <div class="rule-card ${r.enabled ? '' : 'disabled'}" id="rule-card-${r.id}">
      <label class="toggle" title="${r.enabled ? 'Disable' : 'Enable'} rule">
        <input type="checkbox" ${r.enabled ? 'checked' : ''} onchange="toggleRule('${r.id}', this.checked)">
        <span class="toggle-slider"></span>
      </label>
      <div class="rule-meta">
        <div class="rule-name">${escHtml(r.name)}</div>
        <div class="rule-badges">
          <span class="badge badge-trigger">${r.trigger_count || 0} trigger${r.trigger_count !== 1 ? 's' : ''}</span>
          ${r.condition_count ? `<span class="badge badge-cond">${r.condition_count} condition${r.condition_count !== 1 ? 's' : ''}</span>` : ''}
          <span class="badge badge-action">${r.action_count || 0} action${r.action_count !== 1 ? 's' : ''}</span>
          ${r.cooldown ? `<span class="badge">cooldown ${r.cooldown}s</span>` : ''}
        </div>
      </div>
      <div class="rule-fired-time">${r.last_fired ? '⚡ ' + fmtTime(r.last_fired) : 'Never fired'}</div>
      <div class="rule-actions">
        <button class="btn btn-ghost btn-sm btn-icon" onclick="testRule('${r.id}')" title="Test / Fire now">▶</button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="editRule('${r.id}')" title="Edit">✏</button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="duplicateRule('${r.id}')" title="Duplicate">⎘</button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="deleteRule('${r.id}')" title="Delete">🗑</button>
      </div>
    </div>
  `).join('');
}

async function toggleRule(id, enabled) {
  try {
    await API.setEnabled(id, enabled);
    const r = allRules.find(r => r.id === id);
    if (r) r.enabled = enabled;
    renderRules();
  } catch(e) {
    toast('Failed to update rule', 'error');
  }
}

async function testRule(id) {
  try {
    await API.fireRule(id);
    toast('Rule fired', 'success');
    setTimeout(loadRules, 500);
  } catch(e) {
    toast('Failed to fire rule', 'error');
  }
}

async function editRule(id) {
  const rule = await API.getRule(id);
  openRuleEditor(rule);
}

async function duplicateRule(id) {
  try {
    const rule = await API.getRule(id);
    const { id: _id, execution_count: _ec, last_fired: _lf, ...copy } = rule;
    copy.name = copy.name + ' (copy)';
    copy.enabled = false;
    openRuleEditor(copy);
  } catch(e) {
    toast('Failed to duplicate rule', 'error');
  }
}

async function deleteRule(id) {
  const r = allRules.find(r => r.id === id);
  if (!confirm(`Delete rule "${r ? r.name : id}"?`)) return;
  try {
    await API.deleteRule(id);
    toast('Rule deleted', 'success');
    loadRules();
  } catch(e) {
    toast('Failed to delete rule', 'error');
  }
}

/* ===================================================
 * Rule Editor Modal
 * =================================================== */
function openRuleEditor(rule) {
  editingRule = rule;
  triggerRows = rule ? (rule.triggers || []).map(t => ({ ...t })) : [];
  conditionRows = rule ? (rule.conditions || []).map(c => ({ ...c })) : [];
  actionRows = rule ? (rule.actions || []).map(a => ({ ...a })) : [];

  document.getElementById('modal-title').textContent = rule ? 'Edit Rule' : 'New Rule';
  document.getElementById('modal-body').innerHTML = buildRuleForm(rule);
  document.getElementById('modal-overlay').classList.remove('hidden');
  renderTriggerList();
  renderConditionList();
  renderActionList();
}

function closeModal() {
  document.getElementById('modal-overlay').classList.add('hidden');
  editingRule = null;
}

function buildRuleForm(rule) {
  return `
    <div class="form-row">
      <div class="form-group" style="flex:2">
        <label>Rule Name</label>
        <input id="f-name" type="text" value="${rule ? escHtml(rule.name) : ''}" placeholder="e.g. Motion → Record">
      </div>
      <div class="form-group" style="flex:0 0 auto; justify-content:flex-end; padding-top:18px;">
        <label class="toggle" style="align-self:center">
          <input type="checkbox" id="f-enabled" ${(!rule || rule.enabled !== false) ? 'checked' : ''}>
          <span class="toggle-slider"></span>
        </label>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Cooldown (seconds)</label>
        <input id="f-cooldown" type="number" min="0" value="${rule ? rule.cooldown || 0 : 0}" placeholder="0 = no cooldown">
        <div class="form-hint">Minimum seconds between firings</div>
      </div>
      <div class="form-group">
        <label>Max Executions</label>
        <input id="f-maxex" type="number" min="0" value="${rule ? rule.max_executions || 0 : 0}" placeholder="0 = unlimited">
        <div class="form-hint">0 = fire unlimited times</div>
      </div>
    </div>

    <!-- Triggers -->
    <div class="section-header">
      <span class="section-title">Triggers (IF)</span>
      <div class="logic-toggle">
        <button class="logic-btn ${!rule || (rule.trigger_logic||'OR')==='OR' ? 'active' : ''}" onclick="setLogic('trigger','OR',this)">OR</button>
        <button class="logic-btn ${rule && rule.trigger_logic==='AND' ? 'active' : ''}" onclick="setLogic('trigger','AND',this)">AND</button>
      </div>
    </div>
    <div id="trigger-list"></div>
    <button class="add-btn" onclick="addTriggerRow()">+ Add Trigger</button>

    <!-- Conditions -->
    <div class="section-header">
      <span class="section-title">Conditions (WHEN)</span>
      <div class="logic-toggle">
        <button class="logic-btn ${!rule || (rule.condition_logic||'AND')==='AND' ? 'active' : ''}" onclick="setLogic('condition','AND',this)">AND</button>
        <button class="logic-btn ${rule && rule.condition_logic==='OR' ? 'active' : ''}" onclick="setLogic('condition','OR',this)">OR</button>
      </div>
    </div>
    <div id="condition-list"></div>
    <button class="add-btn" onclick="addConditionRow()">+ Add Condition</button>

    <!-- Actions -->
    <div class="section-header">
      <span class="section-title">Actions (THEN)</span>
    </div>
    <div id="action-list"></div>
    <button class="add-btn" onclick="addActionRow()">+ Add Action</button>
  `;
}

/* Logic toggle */
let triggerLogic = 'OR';
let conditionLogic = 'AND';

function setLogic(which, val, btn) {
  const container = btn.closest('.logic-toggle');
  container.querySelectorAll('.logic-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  if (which === 'trigger') triggerLogic = val;
  else conditionLogic = val;
}

/* ===== Trigger rows ===== */
const TRIGGER_TYPES = [
  { value: 'vapix_event',       label: 'VAPIX Event' },
  { value: 'http_webhook',      label: 'HTTP Webhook' },
  { value: 'schedule',          label: 'Schedule' },
  { value: 'mqtt_message',      label: 'MQTT Message' },
  { value: 'io_input',          label: 'I/O Input' },
  { value: 'counter_threshold', label: 'Counter Threshold' },
  { value: 'rule_fired',        label: 'Rule Fired' },
];

function triggerTypeOptions(selected) {
  return TRIGGER_TYPES.map(t =>
    `<option value="${t.value}" ${selected === t.value ? 'selected' : ''}>${t.label}</option>`
  ).join('');
}

function triggerFields(t, rowIdx) {
  const type = t.type || 'vapix_event';
  if (type === 'vapix_event') {
    /* Topic data may be in object form {ns:val} (from applyVapixEvent / saved JSON)
     * or in flat form topic0_ns / topic0_val (after collectTriggerRow reads form inputs).
     * Handle both so rerenderTrigger doesn't lose the selected event. */
    const ns  = k => t[`${k}_ns`]  !== undefined ? t[`${k}_ns`]
                   : (t[k] ? Object.keys(t[k])[0]   || '' : '');
    const val = k => t[`${k}_val`] !== undefined ? t[`${k}_val`]
                   : (t[k] ? Object.values(t[k])[0] || '' : '');
    /* Hidden inputs so normalizeTrigger can still read topic values */
    const hiddenTopics = ['topic0','topic1','topic2','topic3'].map(k =>
      `<input type="hidden" data-k="${k}_ns" value="${escHtml(ns(k))}">` +
      `<input type="hidden" data-k="${k}_val" value="${escHtml(val(k))}">`
    ).join('');

    if (vapixEventCatalog === null) {
      return `${hiddenTopics}
      <div class="form-row"><div class="form-group">
        <label>Camera Event</label>
        <select disabled><option>Loading events from camera…</option></select>
      </div></div>`;
    }

    const matchIdx  = findCatalogMatch(t);
    const dataKeys  = matchIdx >= 0 ? vapixEventCatalog[matchIdx].dataKeys : [];

    /* Determine current condition type from saved fields */
    const condType  = t.cond_type || (t.value_key ? 'numeric' : (t.filter_key ? 'boolean' : 'none'));
    const filterKey = t.filter_key || '';
    const filterVal = t.filter_value;
    const valueKey  = t.value_key  || (dataKeys[0] || '');
    const valueOp   = t.value_op   || 'gt';
    const valueThr  = t.value_threshold !== undefined ? t.value_threshold : '';
    const valueHold = t.value_hold_secs || 0;

    const condRow = dataKeys.length ? `
    <div class="form-row">
      <div class="form-group">
        <label>Value Condition <span style="opacity:.6">(optional — only fire when…)</span></label>
        <select data-k="cond_type" onchange="rerenderTrigger(this)" style="margin-bottom:8px">
          <option value="none"    ${condType==='none'    ?'selected':''}>Fire on every event</option>
          <option value="boolean" ${condType==='boolean' ?'selected':''}>Boolean match (true / false)</option>
          <option value="numeric" ${condType==='numeric' ?'selected':''}>Numeric threshold</option>
        </select>
        ${condType === 'boolean' ? `
        <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
          <select data-k="filter_key">
            <option value="">— pick field —</option>
            ${dataKeys.map(k =>
              `<option value="${escHtml(k)}" ${filterKey===k?'selected':''}>${escHtml(k)}</option>`
            ).join('')}
          </select>
          <select data-k="filter_value">
            <option value="true"  ${filterVal===true ||filterVal==='true' ?'selected':''}>= true</option>
            <option value="false" ${filterVal===false||filterVal==='false'?'selected':''}>= false</option>
          </select>
        </div>` : ''}
        ${condType === 'numeric' ? `
        <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
          <select data-k="value_key">
            ${dataKeys.map(k =>
              `<option value="${escHtml(k)}" ${valueKey===k?'selected':''}>${escHtml(k)}</option>`
            ).join('')}
          </select>
          <select data-k="value_op">
            <option value="gt"  ${valueOp==='gt' ?'selected':''}>is above</option>
            <option value="lt"  ${valueOp==='lt' ?'selected':''}>is below</option>
            <option value="gte" ${valueOp==='gte'?'selected':''}>is above or equal to</option>
            <option value="lte" ${valueOp==='lte'?'selected':''}>is below or equal to</option>
            <option value="eq"  ${valueOp==='eq' ?'selected':''}>equals</option>
          </select>
          <input type="number" data-k="value_threshold" step="any"
                 value="${escHtml(String(valueThr))}" placeholder="0"
                 style="width:90px">
        </div>
        <div style="display:flex;gap:8px;align-items:center;margin-top:6px;flex-wrap:wrap">
          <span style="opacity:.7;font-size:12px">Hold for at least</span>
          <input type="number" data-k="value_hold_secs" min="0" step="1"
                 value="${valueHold}" style="width:70px">
          <span style="opacity:.7;font-size:12px">seconds (0 = fire immediately)</span>
        </div>` : ''}
      </div>
    </div>` : '';

    return `${hiddenTopics}
    <div class="form-row">
      <div class="form-group">
        <label>Camera Event</label>
        <select onchange="applyVapixEvent(${rowIdx}, this.value)">
          <option value="-1" ${matchIdx < 0 ? 'selected':''}>— Choose an event —</option>
          ${vapixEventCatalog.map((ev, i) =>
            `<option value="${i}" ${i===matchIdx?'selected':''}>${escHtml(ev.label)}</option>`
          ).join('')}
        </select>
        ${matchIdx < 0 && (t.topic0 || t.topic1)
          ? `<div class="form-hint" style="color:var(--text-muted)">Saved event not found in this camera's catalog — it may still work if the camera supports it.</div>`
          : `<div class="form-hint">Choose which camera event fires this rule.</div>`}
      </div>
    </div>
    ${condRow}`;
  }
  if (type === 'http_webhook') {
    const tok = t.token || generateToken();
    const base = `${window.location.protocol}//${window.location.hostname}/local/acap_event_engine/fire`;
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Token <span style="opacity:.6">(acts as a secret password for this webhook)</span></label>
        <input type="text" data-k="token" value="${escHtml(tok)}" placeholder="secret-token"
               oninput="updateWebhookUrl(this)">
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Webhook URL — POST to this address to fire the rule</label>
        <input class="webhook-url-display" type="text" readonly
               value="${escHtml(`${base}?token=${tok}`)}"
               style="font-size:11px;font-family:monospace;cursor:pointer;color:var(--accent)"
               onclick="this.select()" title="Click to select all">
        <div class="form-hint">No authentication required. Optionally send a JSON body — values are available as <code>{{trigger.KEY}}</code> in action templates.</div>
      </div>
    </div>`;
  }
  if (type === 'schedule') return `
    <div class="form-row">
      <div class="form-group">
        <label>Schedule Type</label>
        <select data-k="schedule_type" onchange="rerenderTrigger(this)">
          <option value="daily_time" ${(t.schedule_type||'daily_time')==='daily_time' ? 'selected' : ''}>Daily Time</option>
          <option value="interval"   ${t.schedule_type==='interval'   ? 'selected' : ''}>Interval</option>
          <option value="cron"       ${t.schedule_type==='cron'       ? 'selected' : ''}>Cron Expression</option>
        </select>
      </div>
    </div>
    ${(t.schedule_type||'daily_time') === 'daily_time' ? `
    <div class="form-row">
      <div class="form-group">
        <label>Time (HH:MM)</label>
        <input type="time" data-k="time" value="${escHtml(t.time || '08:00')}">
      </div>
      <div class="form-group">
        <label>Days</label>
        <input type="text" data-k="days_str" value="${(t.days || [1,2,3,4,5]).join(',')}" placeholder="0=Sun,1=Mon,...6=Sat">
        <div class="form-hint">Comma-separated day numbers (0=Sun)</div>
      </div>
    </div>` : ''}
    ${t.schedule_type === 'interval' ? `
    <div class="form-row">
      <div class="form-group">
        <label>Interval (seconds)</label>
        <input type="number" data-k="interval_seconds" min="1" value="${t.interval_seconds || 60}">
      </div>
    </div>` : ''}
    ${t.schedule_type === 'cron' ? `
    <div class="form-row">
      <div class="form-group">
        <label>Cron Expression</label>
        <input type="text" data-k="cron" value="${escHtml(t.cron || '0 * * * *')}" placeholder="0 * * * *">
        <div class="form-hint">minute hour dom month dow  (e.g. "0 8 * * 1-5" = weekdays at 08:00)</div>
      </div>
    </div>` : ''}`;
  if (type === 'io_input') return `
    <div class="form-row">
      <div class="form-group">
        <label>Port Number</label>
        <input type="number" data-k="port" min="1" value="${t.port || 1}">
      </div>
      <div class="form-group">
        <label>Edge</label>
        <select data-k="edge">
          <option value="rising"  ${(t.edge||'rising')==='rising'  ? 'selected' : ''}>Rising (activate)</option>
          <option value="falling" ${t.edge==='falling' ? 'selected' : ''}>Falling (deactivate)</option>
          <option value="both"    ${t.edge==='both'    ? 'selected' : ''}>Both</option>
        </select>
      </div>
    </div>`;
  if (type === 'counter_threshold') return `
    <div class="form-row">
      <div class="form-group">
        <label>Counter Name</label>
        <input type="text" data-k="counter_name" value="${escHtml(t.counter_name || '')}" placeholder="my_counter">
      </div>
      <div class="form-group">
        <label>Operator</label>
        <select data-k="op">
          <option value="gt"  ${(t.op||'gt')==='gt'  ? 'selected' : ''}>&gt; (greater than)</option>
          <option value="gte" ${t.op==='gte' ? 'selected' : ''}>&ge; (greater or equal)</option>
          <option value="lt"  ${t.op==='lt'  ? 'selected' : ''}>&lt; (less than)</option>
          <option value="lte" ${t.op==='lte' ? 'selected' : ''}>&le; (less or equal)</option>
          <option value="eq"  ${t.op==='eq'  ? 'selected' : ''}>=  (equal)</option>
        </select>
      </div>
      <div class="form-group">
        <label>Value</label>
        <input type="number" data-k="value" value="${t.value !== undefined ? t.value : 0}">
      </div>
    </div>`;
  if (type === 'rule_fired') return `
    <div class="form-row">
      <div class="form-group">
        <label>Rule ID (leave empty for any rule)</label>
        <input type="text" data-k="rule_id" value="${escHtml(t.rule_id || '')}" placeholder="rule UUID">
      </div>
    </div>`;
  if (type === 'mqtt_message') return `
    <div class="form-row">
      <div class="form-group">
        <label>Topic Filter</label>
        <input type="text" data-k="topic_filter" value="${escHtml(t.topic_filter || '#')}" placeholder="sensors/# or home/+/temperature">
        <div class="form-hint">Supports MQTT wildcards: + (single level) and # (multi-level). Requires MQTT configured in Settings.</div>
      </div>
      <div class="form-group">
        <label>Payload Filter <span style="font-weight:normal;opacity:.7">(optional)</span></label>
        <input type="text" data-k="payload_filter" value="${escHtml(t.payload_filter || '')}" placeholder="e.g. motion or &quot;state&quot;:true">
        <div class="form-hint">Rule fires only if the payload contains this substring. Leave empty to match any payload.</div>
      </div>
    </div>`;
  return '';
}

function renderTriggerList() {
  const list = document.getElementById('trigger-list');
  if (!list) return;
  if (!triggerRows.length) {
    list.innerHTML = '';
    return;
  }
  list.innerHTML = triggerRows.map((t, i) => `
    <div class="tca-row" id="trow-${i}">
      <div class="tca-row-header">
        <span class="tca-type-badge tca-trigger">Trigger ${i + 1}</span>
        <select onchange="changeTriggerType(${i}, this.value)" style="background:var(--bg);border:1px solid var(--border);color:var(--text);padding:4px 8px;border-radius:6px;font-size:12px;">
          ${triggerTypeOptions(t.type || 'vapix_event')}
        </select>
        <button class="tca-remove" onclick="removeTriggerRow(${i})" title="Remove">&times;</button>
      </div>
      <div class="tca-fields">${triggerFields(t, i)}</div>
    </div>
  `).join('');
}

function addTriggerRow() {
  triggerRows.push({ type: 'vapix_event' });
  renderTriggerList();
}

function removeTriggerRow(i) {
  triggerRows.splice(i, 1);
  renderTriggerList();
}

function changeTriggerType(i, type) {
  collectTriggerRow(i);
  triggerRows[i] = { type };
  renderTriggerList();
}

function rerenderTrigger(sel) {
  const row = sel.closest('.tca-row');
  const i = parseInt(row.id.replace('trow-', ''));
  collectTriggerRow(i);
  renderTriggerList();
}

function collectTriggerRow(i) {
  const el = document.getElementById('trow-' + i);
  if (!el) return;
  const type = triggerRows[i].type;
  const data = { type };
  el.querySelectorAll('[data-k]').forEach(inp => {
    data[inp.dataset.k] = inp.value;
  });
  triggerRows[i] = data;
}

/* ===== Condition rows ===== */
const CONDITION_TYPES = [
  { value: 'time_window',      label: 'Time Window' },
  { value: 'counter',          label: 'Counter Compare' },
  { value: 'variable_compare', label: 'Variable Compare' },
  { value: 'io_state',         label: 'I/O State' },
  { value: 'http_check',       label: 'HTTP Check' },
];

function conditionTypeOptions(selected) {
  return CONDITION_TYPES.map(t =>
    `<option value="${t.value}" ${selected === t.value ? 'selected' : ''}>${t.label}</option>`
  ).join('');
}

function conditionFields(c) {
  const type = c.type || 'time_window';
  if (type === 'time_window') return `
    <div class="form-row">
      <div class="form-group">
        <label>Start Time</label>
        <input type="time" data-k="start" value="${escHtml(c.start || '08:00')}">
      </div>
      <div class="form-group">
        <label>End Time</label>
        <input type="time" data-k="end" value="${escHtml(c.end || '18:00')}">
      </div>
      <div class="form-group">
        <label>Days (0=Sun,6=Sat)</label>
        <input type="text" data-k="days_str" value="${(c.days || [1,2,3,4,5]).join(',')}" placeholder="1,2,3,4,5">
      </div>
    </div>`;
  if (type === 'counter') return `
    <div class="form-row">
      <div class="form-group">
        <label>Counter Name</label>
        <input type="text" data-k="name" value="${escHtml(c.name || '')}" placeholder="my_counter">
      </div>
      <div class="form-group">
        <label>Operator</label>
        <select data-k="op">
          <option value="gt"  ${(c.op||'gt')==='gt'  ? 'selected' : ''}>&gt;</option>
          <option value="gte" ${c.op==='gte' ? 'selected' : ''}>&ge;</option>
          <option value="lt"  ${c.op==='lt'  ? 'selected' : ''}>&lt;</option>
          <option value="lte" ${c.op==='lte' ? 'selected' : ''}>&le;</option>
          <option value="eq"  ${c.op==='eq'  ? 'selected' : ''}>=</option>
        </select>
      </div>
      <div class="form-group">
        <label>Value</label>
        <input type="number" data-k="value" value="${c.value !== undefined ? c.value : 0}">
      </div>
    </div>`;
  if (type === 'variable_compare') return `
    <div class="form-row">
      <div class="form-group">
        <label>Variable Name</label>
        <input type="text" data-k="name" value="${escHtml(c.name || '')}" placeholder="my_var">
      </div>
      <div class="form-group">
        <label>Operator</label>
        <select data-k="op">
          <option value="eq"  ${(c.op||'eq')==='eq'  ? 'selected' : ''}>=</option>
          <option value="ne"  ${c.op==='ne'  ? 'selected' : ''}>≠</option>
          <option value="lt"  ${c.op==='lt'  ? 'selected' : ''}>&lt;</option>
          <option value="gt"  ${c.op==='gt'  ? 'selected' : ''}>&gt;</option>
        </select>
      </div>
      <div class="form-group">
        <label>Value</label>
        <input type="text" data-k="value" value="${escHtml(c.value || '')}">
      </div>
    </div>`;
  if (type === 'io_state') return `
    <div class="form-row">
      <div class="form-group">
        <label>Port</label>
        <input type="number" data-k="port" min="1" value="${c.port || 1}">
      </div>
      <div class="form-group">
        <label>Expected State</label>
        <select data-k="state">
          <option value="active"   ${(c.state||'active')==='active'   ? 'selected' : ''}>Active (high)</option>
          <option value="inactive" ${c.state==='inactive' ? 'selected' : ''}>Inactive (low)</option>
        </select>
      </div>
    </div>`;
  if (type === 'http_check') return `
    <div class="form-row">
      <div class="form-group">
        <label>URL</label>
        <input type="text" data-k="url" value="${escHtml(c.url || '')}" placeholder="https://...">
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Expected HTTP Status</label>
        <input type="number" data-k="expected_status" value="${c.expected_status || 200}" placeholder="200">
      </div>
      <div class="form-group">
        <label>Expected Body Contains (optional)</label>
        <input type="text" data-k="expected_body" value="${escHtml(c.expected_body || '')}" placeholder="ok">
      </div>
    </div>`;
  return '';
}

function renderConditionList() {
  const list = document.getElementById('condition-list');
  if (!list) return;
  list.innerHTML = conditionRows.map((c, i) => `
    <div class="tca-row" id="crow-${i}">
      <div class="tca-row-header">
        <span class="tca-type-badge tca-condition">Condition ${i + 1}</span>
        <select onchange="changeConditionType(${i}, this.value)" style="background:var(--bg);border:1px solid var(--border);color:var(--text);padding:4px 8px;border-radius:6px;font-size:12px;">
          ${conditionTypeOptions(c.type || 'time_window')}
        </select>
        <button class="tca-remove" onclick="removeConditionRow(${i})">&times;</button>
      </div>
      <div class="tca-fields">${conditionFields(c)}</div>
    </div>
  `).join('');
}

function addConditionRow() { conditionRows.push({ type: 'time_window' }); renderConditionList(); }
function removeConditionRow(i) { conditionRows.splice(i, 1); renderConditionList(); }
function changeConditionType(i, type) { conditionRows[i] = { type }; renderConditionList(); }

/* ===== Action rows ===== */
const ACTION_TYPES = [
  { value: 'http_request',      label: 'HTTP Request' },
  { value: 'recording',         label: 'Recording' },
  { value: 'overlay_text',      label: 'Overlay Text' },
  { value: 'ptz_preset',        label: 'PTZ Preset' },
  { value: 'io_output',         label: 'I/O Output' },
  { value: 'audio_clip',        label: 'Audio Clip' },
  { value: 'send_syslog',       label: 'Send Syslog' },
  { value: 'vapix_query',       label: 'VAPIX Event Query' },
  { value: 'fire_vapix_event',  label: 'Fire VAPIX Event' },
  { value: 'delay',             label: 'Delay' },
  { value: 'set_variable',      label: 'Set Variable' },
  { value: 'increment_counter', label: 'Increment Counter' },
  { value: 'mqtt_publish',      label: 'MQTT Publish' },
  { value: 'run_rule',          label: 'Run Rule' },
];

function actionTypeOptions(selected) {
  return ACTION_TYPES.map(t =>
    `<option value="${t.value}" ${selected === t.value ? 'selected' : ''}>${t.label}</option>`
  ).join('');
}

function getTriggerTokens() {
  const tokens = ['{{trigger_json}}'];  /* always available — full trigger data as JSON */
  for (const t of triggerRows) {
    if (t.type === 'vapix_event') {
      const idx = findCatalogMatch(t);
      if (idx >= 0 && vapixEventCatalog[idx].dataKeys.length) {
        for (const k of vapixEventCatalog[idx].dataKeys) tokens.push(`{{trigger.${k}}}`);
      } else {
        tokens.push('{{trigger.active}}');
      }
    } else if (t.type === 'mqtt_message') {
      tokens.push('{{trigger.topic}}', '{{trigger.payload}}');
    } else if (t.type === 'http_webhook') {
      tokens.push('{{trigger.source}}');
    } else if (t.type === 'counter_threshold') {
      tokens.push('{{trigger.counter}}', '{{trigger.value}}');
    } else if (t.type === 'io_input') {
      tokens.push('{{trigger.port}}', '{{trigger.state}}');
    }
  }
  return [...new Set(tokens)];
}

/* Token picker — uses a position:fixed panel appended to document.body so it
 * is never clipped by the modal's overflow:auto container. */
let _tokenPickerBtn    = null;  /* the button that opened the current panel */
let _tokenTargetInput  = null;  /* the input/textarea to insert into */

function toggleTokenPicker(btn) {
  const existingPanel = document.getElementById('_token_picker_panel');

  /* clicking same button again → close */
  if (existingPanel && _tokenPickerBtn === btn) {
    existingPanel.remove();
    _tokenPickerBtn = null;
    _tokenTargetInput = null;
    return;
  }
  if (existingPanel) existingPanel.remove();

  /* remember the target input (sibling in same .form-group) */
  const group = btn.closest('.form-group');
  _tokenTargetInput = group && (group.querySelector('textarea') || group.querySelector('input[type="text"]'));
  _tokenPickerBtn   = btn;

  /* build token groups */
  const triggerTokens = getTriggerTokens();
  const groups = [
    { label: 'Time',    tokens: ['{{timestamp}}', '{{date}}', '{{time}}'] },
    { label: 'Camera',  tokens: ['{{camera.serial}}', '{{camera.model}}', '{{camera.ip}}'] },
  ];
  if (triggerTokens.length)
    groups.push({ label: 'Trigger', tokens: triggerTokens });
  if (knownVarNames.length)
    groups.push({ label: 'Variables', tokens: knownVarNames.map(n => `{{var.${n}}}`) });
  if (knownCounterNames.length)
    groups.push({ label: 'Counters', tokens: knownCounterNames.map(n => `{{counter.${n}}}`) });
  else
    groups.push({ label: 'Counters', tokens: ['{{counter.NAME}}'] });

  const row = (t) =>
    `<button type="button" onclick="insertTokenFromPicker(${escHtml(JSON.stringify(t))})"
       style="display:block;width:100%;text-align:left;padding:4px 12px;font-size:11px;
              font-family:monospace;background:none;border:none;color:var(--text);
              cursor:pointer;white-space:nowrap"
       onmouseover="this.style.background='var(--surface2)'"
       onmouseout="this.style.background='none'">${escHtml(t)}</button>`;

  const label = (l) =>
    `<div style="padding:6px 8px 2px;font-size:10px;color:var(--text-dim);
                 font-weight:600;text-transform:uppercase;letter-spacing:.05em">${l}</div>`;

  const panel = document.createElement('div');
  panel.id = '_token_picker_panel';
  panel.style.cssText =
    'position:fixed;z-index:9999;background:var(--surface);border:1px solid var(--border);' +
    'border-radius:6px;max-height:240px;overflow-y:auto;min-width:200px;' +
    'box-shadow:0 4px 16px rgba(0,0,0,.5);padding:4px 0';
  panel.innerHTML = groups.map(g => label(g.label) + g.tokens.map(row).join('')).join('');
  document.body.appendChild(panel);

  /* position below the button, flip left if near right edge */
  const rect = btn.getBoundingClientRect();
  const pw = 210;
  const left = (rect.left + pw > window.innerWidth) ? Math.max(0, rect.right - pw) : rect.left;
  panel.style.left = left + 'px';
  panel.style.top  = (rect.bottom + 4) + 'px';

  /* close on outside click */
  setTimeout(() => {
    document.addEventListener('click', function _close(e) {
      if (!panel.contains(e.target) && e.target !== btn) {
        panel.remove();
        _tokenPickerBtn = null;
        _tokenTargetInput = null;
      }
      document.removeEventListener('click', _close);
    });
  }, 0);
}

function insertTokenFromPicker(token) {
  const target = _tokenTargetInput;
  const panel  = document.getElementById('_token_picker_panel');
  if (panel) panel.remove();
  _tokenPickerBtn   = null;
  _tokenTargetInput = null;
  if (!target) return;
  const s = target.selectionStart ?? target.value.length;
  const e = target.selectionEnd   ?? s;
  target.value = target.value.slice(0, s) + token + target.value.slice(e);
  target.selectionStart = target.selectionEnd = s + token.length;
  target.focus();
}

function getTokenInsertWidget() {
  return `<div class="form-hint">
    <button type="button" class="btn btn-ghost btn-sm"
            onclick="toggleTokenPicker(this)"
            style="font-size:11px;padding:2px 8px;font-family:monospace">{ } Insert token</button>
  </div>`;
}

function actionFields(a) {
  const type = a.type || 'http_request';
  const hint = getTokenInsertWidget();
  if (type === 'http_request') return `
    <div class="form-row">
      <div class="form-group">
        <label>URL</label>
        <input type="text" data-k="url" value="${escHtml(a.url || '')}" placeholder="https://...">
        ${hint}
      </div>
    </div>
    <div class="form-row">
      <div class="form-group" style="flex:0 0 120px;">
        <label>Method</label>
        <select data-k="method">
          <option value="GET"  ${(a.method||'GET')==='GET'  ? 'selected' : ''}>GET</option>
          <option value="POST" ${a.method==='POST' ? 'selected' : ''}>POST</option>
          <option value="PUT"  ${a.method==='PUT'  ? 'selected' : ''}>PUT</option>
          <option value="DELETE" ${a.method==='DELETE' ? 'selected' : ''}>DELETE</option>
        </select>
      </div>
      <div class="form-group">
        <label>Headers (one per line, key: value)</label>
        <textarea data-k="headers" placeholder="Content-Type: application/json">${escHtml(a.headers || '')}</textarea>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Body</label>
        <textarea data-k="body" placeholder='{"key": "{{trigger.value}}"}'>${escHtml(a.body || '')}</textarea>
        ${hint}
      </div>
    </div>`;
  if (type === 'recording') return `
    <div class="form-row">
      <div class="form-group">
        <label>Operation</label>
        <select data-k="operation">
          <option value="start" ${(a.operation||'start')==='start' ? 'selected' : ''}>Start Recording</option>
          <option value="stop"  ${a.operation==='stop' ? 'selected' : ''}>Stop Recording</option>
        </select>
      </div>
      <div class="form-group">
        <label>Max Duration (seconds, 0=unlimited)</label>
        <input type="number" data-k="duration" min="0" value="${a.duration || 0}">
      </div>
    </div>`;
  if (type === 'overlay_text') return `
    <div class="form-row">
      <div class="form-group">
        <label>Text</label>
        <input type="text" data-k="text" value="${escHtml(a.text || '')}" placeholder="MOTION DETECTED at {{time}}">
        ${hint}
      </div>
    </div>
    <div class="form-row">
      <div class="form-group" style="flex:0 0 80px;">
        <label>Channel</label>
        <input type="number" data-k="channel" min="1" value="${a.channel || 1}">
      </div>
      <div class="form-group" style="flex:0 0 130px;">
        <label>Duration (s, 0=keep)</label>
        <input type="number" data-k="duration" min="0" value="${a.duration || 0}">
      </div>
      <div class="form-group" style="flex:0 0 140px;">
        <label>Position</label>
        <select data-k="position">
          <option value="topLeft"     ${(a.position||'topLeft')==='topLeft'     ? 'selected':''}>Top Left</option>
          <option value="topRight"    ${a.position==='topRight'    ? 'selected':''}>Top Right</option>
          <option value="bottomLeft"  ${a.position==='bottomLeft'  ? 'selected':''}>Bottom Left</option>
          <option value="bottomRight" ${a.position==='bottomRight' ? 'selected':''}>Bottom Right</option>
          <option value="top"         ${a.position==='top'         ? 'selected':''}>Top Centre</option>
          <option value="bottom"      ${a.position==='bottom'      ? 'selected':''}>Bottom Centre</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 120px;">
        <label>Text Colour</label>
        <select data-k="text_color">
          <option value="white"         ${(a.text_color||'white')==='white'         ? 'selected':''}>White</option>
          <option value="black"         ${a.text_color==='black'         ? 'selected':''}>Black</option>
          <option value="red"           ${a.text_color==='red'           ? 'selected':''}>Red</option>
          <option value="transparent"   ${a.text_color==='transparent'   ? 'selected':''}>Transparent</option>
          <option value="semiTransparent" ${a.text_color==='semiTransparent' ? 'selected':''}>Semi-transparent</option>
        </select>
      </div>
    </div>`;
  if (type === 'ptz_preset') {
    let presetControl, channelControl = '';
    if (ptzPresets === null) {
      presetControl = `<input type="text" data-k="preset" value="${escHtml(a.preset || '')}" placeholder="Loading presets…" disabled>`;
      channelControl = `<div class="form-group" style="flex:0 0 100px;"><label>Channel</label><input type="number" data-k="channel" min="1" value="${a.channel || 1}"></div>`;
    } else if (!ptzPresets.length) {
      presetControl = `<input type="text" data-k="preset" value="${escHtml(a.preset || '')}" placeholder="HomePosition">`;
      channelControl = `<div class="form-group" style="flex:0 0 100px;"><label>Channel</label><input type="number" data-k="channel" min="1" value="${a.channel || 1}"></div>`;
    } else {
      let opts = `<option value=":">— select preset —</option>`;
      for (const ch of ptzPresets) {
        opts += `<optgroup label="Camera ${ch.channel}">`;
        for (const name of ch.presets) {
          const sel = (a.preset === name && (parseInt(a.channel) || 1) === ch.channel) ? 'selected' : '';
          opts += `<option value="${ch.channel}:${escHtml(name)}" ${sel}>${escHtml(name)}</option>`;
        }
        opts += `</optgroup>`;
      }
      presetControl = `<select data-k="ptz_combined" onchange="applyPtzPreset(this)">${opts}</select>
        <input type="hidden" data-k="preset"  value="${escHtml(a.preset || '')}">
        <input type="hidden" data-k="channel" value="${a.channel || 1}">`;
    }
    return `
    <div class="form-row">
      <div class="form-group"><label>Preset</label>${presetControl}</div>
      ${channelControl}
    </div>`;
  }
  if (type === 'io_output') return `
    <div class="form-row">
      <div class="form-group">
        <label>Port</label>
        <input type="number" data-k="port" min="1" value="${a.port || 1}">
      </div>
      <div class="form-group">
        <label>State</label>
        <select data-k="state">
          <option value="active"   ${(a.state||'active')==='active'   ? 'selected' : ''}>Active</option>
          <option value="inactive" ${a.state==='inactive' ? 'selected' : ''}>Inactive</option>
        </select>
      </div>
      <div class="form-group">
        <label>Duration (seconds, 0=permanent)</label>
        <input type="number" data-k="duration" min="0" value="${a.duration || 0}">
      </div>
    </div>`;
  if (type === 'audio_clip') {
    let clipControl;
    if (audioClips === null) {
      clipControl = `<input type="text" data-k="clip_name" value="${escHtml(a.clip_name || '')}" placeholder="Loading clips…" disabled>`;
    } else if (!audioClips.length) {
      clipControl = `<input type="text" data-k="clip_name" value="${escHtml(a.clip_name || '')}" placeholder="Clip ID (e.g. 36)">`;
    } else {
      let opts = `<option value="">— select clip —</option>`;
      for (const c of audioClips) {
        const sel = a.clip_name === c.id ? 'selected' : '';
        opts += `<option value="${escHtml(c.id)}" ${sel}>${escHtml(c.name)}</option>`;
      }
      clipControl = `<select data-k="clip_name">${opts}</select>`;
    }
    return `
    <div class="form-row">
      <div class="form-group"><label>Audio Clip</label>${clipControl}</div>
    </div>`;
  }
  if (type === 'send_syslog') return `
    <div class="form-row">
      <div class="form-group">
        <label>Message</label>
        <input type="text" data-k="message" value="${escHtml(a.message || '')}" placeholder="Event fired at {{timestamp}}">
        ${hint}
      </div>
      <div class="form-group" style="flex:0 0 120px;">
        <label>Level</label>
        <select data-k="level">
          <option value="info"    ${(a.level||'info')==='info'    ? 'selected' : ''}>Info</option>
          <option value="warning" ${a.level==='warning' ? 'selected' : ''}>Warning</option>
          <option value="error"   ${a.level==='error'   ? 'selected' : ''}>Error</option>
        </select>
      </div>
    </div>`;
  if (type === 'vapix_query') {
    /* Hidden topic inputs — mirroring the trigger pattern */
    const ns  = k => a[k] ? Object.keys(a[k])[0]   || '' : '';
    const val = k => a[k] ? Object.values(a[k])[0] || '' : '';
    const hiddenTopics = ['topic0','topic1','topic2','topic3'].map(k =>
      `<input type="hidden" data-k="${k}_ns" value="${escHtml(ns(k))}">` +
      `<input type="hidden" data-k="${k}_val" value="${escHtml(val(k))}">`
    ).join('');
    const matchIdx = vapixEventCatalog
      ? vapixEventCatalog.findIndex((ev, _i) => {
          const keys = ['topic0','topic1','topic2','topic3'];
          const cmp = (x, y) => {
            if (!x && !y) return true; if (!x || !y) return false;
            const ak = Object.keys(x)[0], bk = Object.keys(y)[0];
            return ak === bk && x[ak] === y[bk];
          };
          return keys.every(k => cmp(ev.topics[k], a[k]));
        })
      : -1;
    return `${hiddenTopics}
    <div class="form-row">
      <div class="form-group">
        <label>Camera Event to Query</label>
        <select onchange="applyVapixEventAction(this)">
          <option value="-1" ${matchIdx < 0 ? 'selected':''}>— Choose an event —</option>
          ${(vapixEventCatalog || []).map((ev, i) =>
            `<option value="${i}" ${i===matchIdx?'selected':''}>${escHtml(ev.label)}</option>`
          ).join('')}
        </select>
        <div class="form-hint">Fetches the latest data from this event and injects it as <code>{{trigger.FIELD}}</code> tokens for all subsequent actions in this rule. Useful with a Schedule trigger to poll current sensor values on demand.</div>
      </div>
    </div>`;
  }
  if (type === 'fire_vapix_event') return `
    <div class="form-row">
      <div class="form-group">
        <label>Event ID</label>
        <input type="text" data-k="event_id" value="${escHtml(a.event_id || '')}" placeholder="RuleFired">
      </div>
      <div class="form-group">
        <label>State (for stateful events)</label>
        <select data-k="state">
          <option value=""      ${!a.state ? 'selected' : ''}>Stateless (pulse)</option>
          <option value="true"  ${a.state === true || a.state === 'true'  ? 'selected' : ''}>High (1)</option>
          <option value="false" ${a.state === false || a.state === 'false' ? 'selected' : ''}>Low (0)</option>
        </select>
      </div>
    </div>`;
  if (type === 'delay') return `
    <div class="form-row">
      <div class="form-group">
        <label>Wait (seconds)</label>
        <input type="number" data-k="seconds" min="1" value="${a.seconds || 5}">
        <div class="form-hint">Actions after this row execute after the delay</div>
      </div>
    </div>`;
  if (type === 'set_variable') return `
    <div class="form-row">
      <div class="form-group">
        <label>Variable Name</label>
        <input type="text" data-k="name" value="${escHtml(a.name || '')}" placeholder="my_var">
      </div>
      <div class="form-group">
        <label>Value</label>
        <input type="text" data-k="value" value="${escHtml(a.value || '')}" placeholder="{{trigger.data}}">
        ${hint}
      </div>
    </div>`;
  if (type === 'increment_counter') return `
    <div class="form-row">
      <div class="form-group">
        <label>Counter Name</label>
        <input type="text" data-k="name" value="${escHtml(a.name || '')}" placeholder="my_counter">
      </div>
      <div class="form-group">
        <label>Operation</label>
        <select data-k="operation">
          <option value="increment" ${(a.operation||'increment')==='increment' ? 'selected' : ''}>Increment</option>
          <option value="decrement" ${a.operation==='decrement' ? 'selected' : ''}>Decrement</option>
          <option value="reset"     ${a.operation==='reset'     ? 'selected' : ''}>Reset to 0</option>
          <option value="set"       ${a.operation==='set'       ? 'selected' : ''}>Set to value</option>
        </select>
      </div>
      <div class="form-group">
        <label>Delta / Value</label>
        <input type="number" data-k="delta" value="${a.delta !== undefined ? a.delta : 1}">
      </div>
    </div>`;
  if (type === 'mqtt_publish') return `
    <div class="form-row">
      <div class="form-group">
        <label>Topic</label>
        <input type="text" data-k="topic" value="${escHtml(a.topic || '')}" placeholder="home/alerts/motion">
        ${hint}
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Payload</label>
        <textarea data-k="payload" placeholder='{"camera":"{{camera.serial}}","time":"{{timestamp}}"}'>${escHtml(a.payload || '')}</textarea>
        ${hint}
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>QoS</label>
        <select data-k="qos">
          <option value="0" ${(a.qos === 0 || a.qos === undefined) ? 'selected':''}>0 – At most once</option>
          <option value="1" ${a.qos === 1 ? 'selected':''}>1 – At least once</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>Retain</label>
        <select data-k="retain">
          <option value="false" ${!a.retain ? 'selected':''}>No</option>
          <option value="true"  ${ a.retain ? 'selected':''}>Yes</option>
        </select>
      </div>
    </div>`;
  if (type === 'run_rule') return `
    <div class="form-row">
      <div class="form-group">
        <label>Target Rule</label>
        <select data-k="rule_id">
          <option value="">Select a rule...</option>
          ${allRules.map(r => `<option value="${r.id}" ${a.rule_id === r.id ? 'selected' : ''}>${escHtml(r.name)}</option>`).join('')}
        </select>
      </div>
    </div>`;
  return '';
}

function renderActionList() {
  const list = document.getElementById('action-list');
  if (!list) return;
  list.innerHTML = actionRows.map((a, i) => `
    <div class="tca-row" id="arow-${i}">
      <div class="tca-row-header">
        <span class="tca-type-badge tca-action">Action ${i + 1}</span>
        <select onchange="changeActionType(${i}, this.value)" style="background:var(--bg);border:1px solid var(--border);color:var(--text);padding:4px 8px;border-radius:6px;font-size:12px;">
          ${actionTypeOptions(a.type || 'http_request')}
        </select>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="moveAction(${i}, -1)" title="Move up" ${i === 0 ? 'disabled' : ''}>↑</button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="moveAction(${i},  1)" title="Move down" ${i === actionRows.length - 1 ? 'disabled' : ''}>↓</button>
        <button class="tca-remove" onclick="removeActionRow(${i})">&times;</button>
      </div>
      <div class="tca-fields">${actionFields(a)}</div>
    </div>
  `).join('');
}

function addActionRow() { actionRows.push({ type: 'http_request' }); renderActionList(); }
function removeActionRow(i) { actionRows.splice(i, 1); renderActionList(); }
function changeActionType(i, type) { actionRows[i] = { type }; renderActionList(); }
function moveAction(i, dir) {
  const j = i + dir;
  if (j < 0 || j >= actionRows.length) return;
  [actionRows[i], actionRows[j]] = [actionRows[j], actionRows[i]];
  renderActionList();
}

/* ===== Collect form data ===== */
function collectRows(rows, prefix) {
  return rows.map((_, i) => {
    const el = document.getElementById(`${prefix}-${i}`);
    if (!el) return _;
    const data = { type: _.type };
    el.querySelectorAll('[data-k]').forEach(inp => {
      data[inp.dataset.k] = inp.value;
    });
    return data;
  });
}

function normalizeTrigger(t) {
  const out = { type: t.type };
  if (t.type === 'vapix_event' || t.type === 'io_input') {
    /* namespace (ns) can legitimately be empty string — only skip if value is absent */
    if (t.topic0_val) out.topic0 = { [t.topic0_ns || '']: t.topic0_val };
    if (t.topic1_val) out.topic1 = { [t.topic1_ns || '']: t.topic1_val };
    if (t.topic2_val) out.topic2 = { [t.topic2_ns || '']: t.topic2_val };
    if (t.topic3_val) out.topic3 = { [t.topic3_ns || '']: t.topic3_val };
    const condType = t.cond_type || (t.value_key ? 'numeric' : (t.filter_key ? 'boolean' : 'none'));
    if (condType === 'boolean') {
      if (t.filter_key) out.filter_key = t.filter_key;
      if (t.filter_value === 'true' || t.filter_value === true)   out.filter_value = true;
      else if (t.filter_value === 'false' || t.filter_value === false) out.filter_value = false;
    } else if (condType === 'numeric') {
      if (t.value_key) out.value_key = t.value_key;
      out.value_op        = t.value_op || 'gt';
      out.value_threshold = parseFloat(t.value_threshold) || 0;
      const hold = parseInt(t.value_hold_secs) || 0;
      if (hold > 0) out.value_hold_secs = hold;
    }
    if (t.type === 'io_input') { out.port = parseInt(t.port) || 1; out.edge = t.edge || 'rising'; }
  } else if (t.type === 'http_webhook') {
    out.token = t.token || '';
  } else if (t.type === 'schedule') {
    out.schedule_type = t.schedule_type || 'daily_time';
    if (out.schedule_type === 'daily_time') {
      out.time = t.time || '08:00';
      out.days = (t.days_str || '1,2,3,4,5').split(',').map(Number).filter(n => n >= 0 && n <= 6);
    } else if (out.schedule_type === 'interval') {
      out.interval_seconds = parseInt(t.interval_seconds) || 60;
    } else if (out.schedule_type === 'cron') {
      out.cron = t.cron || '0 * * * *';
    }
  } else if (t.type === 'mqtt_message') {
    out.topic_filter = t.topic_filter || '#';
    if (t.payload_filter) out.payload_filter = t.payload_filter;
  } else if (t.type === 'counter_threshold') {
    out.counter_name = t.counter_name; out.op = t.op || 'gt'; out.value = parseFloat(t.value) || 0;
  } else if (t.type === 'rule_fired') {
    out.rule_id = t.rule_id || '';
  }
  return out;
}

function normalizeCondition(c) {
  const out = { type: c.type };
  if (c.type === 'time_window') {
    out.start = c.start; out.end = c.end;
    out.days = (c.days_str || '1,2,3,4,5').split(',').map(Number).filter(n => n >= 0 && n <= 6);
  } else if (c.type === 'counter') {
    out.name = c.name; out.op = c.op || 'gt'; out.value = parseFloat(c.value) || 0;
  } else if (c.type === 'variable_compare') {
    out.name = c.name; out.op = c.op || 'eq'; out.value = c.value;
  } else if (c.type === 'io_state') {
    out.port = parseInt(c.port) || 1; out.state = c.state || 'active';
  } else if (c.type === 'http_check') {
    out.url = c.url; out.expected_status = parseInt(c.expected_status) || 200;
    if (c.expected_body) out.expected_body = c.expected_body;
  }
  return out;
}

function normalizeAction(a) {
  const out = { type: a.type };
  const pass = ['url','method','headers','body','operation','duration','text','channel',
                 'preset','port','state','clip_name','message','level','event_id',
                 'seconds','name','value','delta','rule_id','cron','interval_seconds',
                 'schedule_type','time','counter_name','op','token','edge',
                 'topic','payload','qos','position','text_color'];
  pass.forEach(k => { if (a[k] !== undefined && a[k] !== '') out[k] = a[k]; });
  if (out.duration !== undefined) out.duration = parseInt(out.duration) || 0;
  if (out.seconds  !== undefined) out.seconds  = parseInt(out.seconds)  || 1;
  if (out.port     !== undefined) out.port     = parseInt(out.port)     || 1;
  if (out.channel  !== undefined) out.channel  = parseInt(out.channel)  || 1;
  if (out.delta    !== undefined) out.delta    = parseFloat(out.delta)  || 1;
  if (out.value    !== undefined && a.type === 'increment_counter')
    out.delta = parseFloat(out.value) || 0;
  if (a.type === 'fire_vapix_event' && a.state !== '') {
    out.state = a.state === 'true';
  }
  if (a.type === 'mqtt_publish') {
    out.retain = a.retain === 'true' || a.retain === true;
    out.qos    = parseInt(a.qos) || 0;
  }
  if (a.type === 'vapix_query') {
    /* Reconstruct topic0-3 objects from the hidden _ns / _val fields */
    ['topic0','topic1','topic2','topic3'].forEach(k => {
      const v = a[`${k}_val`];
      if (v) out[k] = { [a[`${k}_ns`] || '']: v };
    });
  }
  return out;
}

async function saveRule() {
  // Collect all form values
  triggerRows   = collectRows(triggerRows,   'trow').map(normalizeTrigger);
  conditionRows = collectRows(conditionRows, 'crow').map(normalizeCondition);
  actionRows    = collectRows(actionRows,    'arow').map(normalizeAction);

  const name = document.getElementById('f-name').value.trim();
  if (!name) { toast('Rule name is required', 'error'); return; }
  if (!triggerRows.length) { toast('At least one trigger is required', 'error'); return; }
  if (!actionRows.length) { toast('At least one action is required', 'error'); return; }

  const logicActive = (container, val) => {
    if (!container) return val;
    const active = container.querySelector('.logic-btn.active');
    return active ? active.textContent.trim() : val;
  };

  const rule = {
    name,
    enabled:         document.getElementById('f-enabled').checked,
    triggers:        triggerRows,
    trigger_logic:   triggerLogic,
    conditions:      conditionRows,
    condition_logic: conditionLogic,
    actions:         actionRows,
    cooldown:        parseInt(document.getElementById('f-cooldown').value) || 0,
    max_executions:  parseInt(document.getElementById('f-maxex').value)    || 0,
  };

  try {
    if (editingRule && editingRule.id) {
      await API.updateRule(editingRule.id, rule);
      toast('Rule updated', 'success');
    } else {
      await API.addRule(rule);
      toast('Rule created', 'success');
    }
    closeModal();
    loadRules();
  } catch(e) {
    toast('Failed to save rule: ' + e.message, 'error');
  }
}

/* ===================================================
 * Event Log Tab
 * =================================================== */
async function loadEvents() {
  const filterEl = document.getElementById('log-rule-filter');
  const rule = filterEl ? filterEl.value : '';
  try {
    const events = await API.getEvents(200, rule);
    renderEventLog(events);
  } catch(e) {
    toast('Failed to load events', 'error');
  }
}

function renderEventLog(events) {
  const tbody = document.getElementById('event-log-body');
  if (!events.length) {
    tbody.innerHTML = '<tr><td colspan="4" style="text-align:center;padding:30px;color:var(--text-muted);">No events yet</td></tr>';
    return;
  }
  tbody.innerHTML = events.map(e => {
    const d = new Date(e.timestamp * 1000);
    const time = d.toLocaleTimeString([], { hour12: false }) + ' ' +
                 d.toLocaleDateString([], { month: 'short', day: 'numeric' });
    const badge = e.fired
      ? '<span class="badge event-badge-fired">Fired</span>'
      : `<span class="badge event-badge-blocked">${escHtml(e.block_reason || 'Blocked')}</span>`;
    const details = e.trigger_data
      ? `<code style="font-size:11px;color:var(--text-muted)">${escHtml(JSON.stringify(e.trigger_data).slice(0,80))}</code>`
      : '';
    return `<tr>
      <td style="white-space:nowrap;color:var(--text-muted)">${time}</td>
      <td>${escHtml(e.rule_name || e.rule_id)}</td>
      <td>${badge}</td>
      <td>${details}</td>
    </tr>`;
  }).join('');
}

function updateLogFilter() {
  const sel = document.getElementById('log-rule-filter');
  if (!sel) return;
  const cur = sel.value;
  sel.innerHTML = '<option value="">All Rules</option>' +
    allRules.map(r => `<option value="${r.id}" ${cur === r.id ? 'selected' : ''}>${escHtml(r.name)}</option>`).join('');
}

/* ===================================================
 * Variables Tab
 * =================================================== */
async function loadVariables() {
  try {
    const vars = await API.getVariables();
    renderVariables(vars);
  } catch(e) {
    toast('Failed to load variables', 'error');
  }
}

function renderVariables(vars) {
  const tbody = document.getElementById('variables-body');
  const keys = Object.keys(vars);
  if (!keys.length) {
    tbody.innerHTML = '<tr><td colspan="3" style="text-align:center;padding:30px;color:var(--text-muted);">No variables set</td></tr>';
    return;
  }
  tbody.innerHTML = keys.map(k => `
    <tr>
      <td><code>${escHtml(k)}</code></td>
      <td class="var-value-cell">${escHtml(String(vars[k]))}</td>
      <td style="text-align:right;">
        <button class="btn btn-ghost btn-sm" onclick="deleteVariable('${escHtml(k)}')">Delete</button>
      </td>
    </tr>
  `).join('');
}

async function deleteVariable(name) {
  if (!confirm(`Delete variable "${name}"?`)) return;
  try {
    await API.deleteVariable(name);
    toast('Variable deleted', 'success');
    loadVariables();
  } catch(e) {
    toast('Failed', 'error');
  }
}

/* ===================================================
 * Status Tab
 * =================================================== */
async function loadStatus() {
  try {
    const s = await API.getStatus();
    const grid = document.getElementById('status-grid');
    grid.innerHTML = [
      { label: 'Rules Total',    value: s.rules_total },
      { label: 'Rules Enabled',  value: s.rules_enabled },
      { label: 'Events Today',   value: s.events_today },
      { label: 'Uptime',         value: formatUptime(s.uptime) },
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
  } catch(e) {
    toast('Failed to load status', 'error');
  }
}

function formatUptime(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return h > 0 ? `${h}h ${m}m` : `${m}m`;
}

/* ===================================================
 * MQTT tab
 * =================================================== */
function updateMqttStatusBadge(mq) {
  const dot  = document.getElementById('mqtt-dot');
  const text = document.getElementById('mqtt-status-text');
  if (!dot || !text) return;
  if (mq.connected) {
    dot.style.background = 'var(--accent-success)';
    text.textContent = `Connected — ${escHtml(mq.host || '')}:${mq.port || 1883}`;
  } else if (mq.enabled) {
    dot.style.background = 'var(--accent-warning, #f59e0b)';
    text.textContent = 'Connecting…';
  } else {
    dot.style.background = 'var(--text-dim, #555)';
    text.textContent = 'Disabled';
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
      keepalive: parseInt(form.querySelector('[name="keepalive"]').value, 10) || 60
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

/* ===================================================
 * Export / Import
 * =================================================== */
function downloadJSON(filename, data) {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename; a.click();
  URL.revokeObjectURL(url);
}

async function exportRules() {
  try {
    const rules = await API.getRules();
    downloadJSON('event_engine_rules.json', rules);
  } catch(e) {
    toast('Export failed: ' + e.message, 'error');
  }
}

async function importRules(input) {
  const file = input.files[0];
  if (!file) return;
  input.value = '';
  try {
    const text = await file.text();
    const rules = JSON.parse(text);
    const list = Array.isArray(rules) ? rules : [rules];
    let ok = 0, fail = 0;
    for (const rule of list) {
      /* Strip the id so the engine assigns a new one */
      const { id: _id, ...r } = rule;
      const resp = await API.addRule(r);
      if (resp.ok) ok++; else fail++;
    }
    toast(`Imported ${ok} rule(s)${fail ? ', ' + fail + ' failed' : ''}`);
    loadRules();
  } catch(e) {
    toast('Import failed: ' + e.message, 'error');
  }
}

async function exportSettings() {
  try {
    const settings = await API.get('settings');
    downloadJSON('event_engine_settings.json', settings);
  } catch(e) {
    toast('Export failed: ' + e.message, 'error');
  }
}

async function importSettings(input) {
  const file = input.files[0];
  if (!file) return;
  input.value = '';
  try {
    const text = await file.text();
    const settings = JSON.parse(text);
    const resp = await API.post('settings', settings);
    if (!resp.ok) throw new Error(await resp.text());
    toast('Settings imported — reloading…');
    setTimeout(loadStatus, 500);
    setTimeout(loadMqttSettings, 600);
  } catch(e) {
    toast('Import failed: ' + e.message, 'error');
  }
}

/* ===================================================
 * Misc helpers
 * =================================================== */
function generateToken() {
  return Math.random().toString(36).slice(2, 10) + Math.random().toString(36).slice(2, 10);
}

/* ===================================================
 * Auto-refresh
 * =================================================== */
function startPoll() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(() => {
    const activeTab = document.querySelector('.tab-btn.active');
    if (!activeTab) return;
    const tab = activeTab.dataset.tab;
    if (tab === 'rules')  loadRules();
    else if (tab === 'log')    loadEvents();
    else if (tab === 'status') loadStatus();
    /* mqtt tab: only refresh the status badge, not the form */
    else if (tab === 'mqtt') API.getStatus().then(s => updateMqttStatusBadge((s && s.mqtt) || {})).catch(() => {});
  }, 10000);
}

/* ===================================================
 * Init
 * =================================================== */
window.addEventListener('DOMContentLoaded', () => {
  loadRules();
  loadStatus();
  startPoll();
  loadVapixEventCatalog();
  loadPtzPresets();
  loadAudioClips();
  API.getVariables().then(v => {
    knownVarNames     = Object.keys(v || {}).filter(k => !(v[k] && v[k].is_counter));
    knownCounterNames = Object.keys(v || {}).filter(k =>   v[k] && v[k].is_counter);
  }).catch(() => {});
});
