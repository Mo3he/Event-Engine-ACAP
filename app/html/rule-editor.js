'use strict';

function openRuleEditor(rule) {
  editingRule = rule;
  triggerLogic = rule && rule.trigger_logic ? rule.trigger_logic : 'OR';
  conditionLogic = rule && rule.condition_logic ? rule.condition_logic : 'AND';
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
        <label>Cooldown (s, 0 = off)</label>
        <input id="f-cooldown" type="number" min="0" value="${rule ? rule.cooldown || 0 : 0}" placeholder="0 = no cooldown">
        <div class="form-hint">Prevents the rule from firing again for this many seconds after it triggers. Useful to avoid alert floods.</div>
      </div>
      <div class="form-group">
        <label>Max Executions</label>
        <div style="display:flex;gap:6px;align-items:center;">
          <input id="f-maxex" type="number" min="0" value="${rule ? rule.max_executions || 0 : 0}" placeholder="0 = unlimited" style="width:80px;">
          <select id="f-maxex-period">
            <option value=""       ${!(rule && rule.max_exec_period) ? 'selected' : ''}>lifetime</option>
            <option value="minute" ${rule && rule.max_exec_period === 'minute' ? 'selected' : ''}>per minute</option>
            <option value="hour"   ${rule && rule.max_exec_period === 'hour'   ? 'selected' : ''}>per hour</option>
            <option value="day"    ${rule && rule.max_exec_period === 'day'    ? 'selected' : ''}>per day</option>
          </select>
        </div>
        <div class="form-hint">0 = unlimited. Period resets automatically; lifetime resets when rule is saved.</div>
      </div>
    </div>

    <!-- Triggers -->
    <div class="section-header">
      <span class="section-title">Triggers (IF)</span>
      <div class="logic-toggle">
        <button class="logic-btn ${!rule || (rule.trigger_logic||'OR')==='OR' ? 'active' : ''}" onclick="setLogic('trigger','OR',this)">OR</button>
        <button class="logic-btn ${rule && (rule.trigger_logic==='AND' || rule.trigger_logic==='AND_ACTIVE') ? 'active' : ''}" onclick="setLogic('trigger','AND',this)">AND</button>
        <span style="margin-left:8px;font-size:10px;color:var(--text-dim);">OR = any trigger fires | AND = all must activate</span>
      </div>
    </div>
    <div id="trigger-and-options" style="display:${rule && (rule.trigger_logic==='AND' || rule.trigger_logic==='AND_ACTIVE') ? 'block' : 'none'};">
      <div style="display:flex;gap:16px;align-items:center;margin-bottom:8px;">
        <label style="display:flex;align-items:center;gap:4px;font-size:12px;cursor:pointer;">
          <input type="radio" name="and-mode" value="AND_ACTIVE" ${!rule || rule.trigger_logic !== 'AND' ? 'checked' : ''} onchange="setAndMode(this.value)">
          All active simultaneously
        </label>
        <label style="display:flex;align-items:center;gap:4px;font-size:12px;cursor:pointer;">
          <input type="radio" name="and-mode" value="AND" ${rule && rule.trigger_logic === 'AND' ? 'checked' : ''} onchange="setAndMode(this.value)">
          Correlation window
        </label>
      </div>
      <div class="form-hint" style="margin-bottom:8px;">Simultaneous = all triggers in their active state at the same time. Correlation = all must fire within a time window.</div>
      <div id="trigger-window-row" style="display:${rule && rule.trigger_logic==='AND' ? 'flex' : 'none'};gap:16px;align-items:flex-start;margin-bottom:8px;">
        <div class="form-group" style="flex:0 0 260px;">
          <label>Correlation Window (s, 0 = no time limit)</label>
          <input id="f-trigger-window" type="number" min="0" value="${rule && rule.trigger_window ? rule.trigger_window : 0}" placeholder="0 = no time limit">
          <div class="form-hint">All triggers must fire within this time. 0 = no time limit.</div>
        </div>
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
        <span style="margin-left:8px;font-size:10px;color:var(--text-dim);">AND = all must pass | OR = any passing allows fire</span>
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
  if (which === 'trigger') {
    const andOpts = document.getElementById('trigger-and-options');
    if (val === 'AND') {
      triggerLogic = 'AND_ACTIVE';
      if (andOpts) andOpts.style.display = 'block';
      const activeRadio = document.querySelector('input[name="and-mode"][value="AND_ACTIVE"]');
      if (activeRadio) activeRadio.checked = true;
      const twRow = document.getElementById('trigger-window-row');
      if (twRow) twRow.style.display = 'none';
    } else {
      triggerLogic = 'OR';
      if (andOpts) andOpts.style.display = 'none';
    }
  } else {
    conditionLogic = val;
  }
}

function setAndMode(val) {
  triggerLogic = val;
  const twRow = document.getElementById('trigger-window-row');
  if (twRow) twRow.style.display = val === 'AND' ? 'flex' : 'none';
}

/* ===== Trigger rows ===== */
const TRIGGER_GROUPS = [
  { label: 'Camera / Device', types: [
    { value: 'vapix_event',       label: 'Device Event' },
    { value: 'io_input',          label: 'I/O Input' },
    { value: 'aoa_scenario',      label: 'AOA Scenario' },
  ]},
  { label: 'Time', types: [
    { value: 'schedule',          label: 'Schedule' },
  ]},
  { label: 'External', types: [
    { value: 'mqtt_message',      label: 'MQTT Message' },
    { value: 'http_webhook',      label: 'HTTP Webhook' },
  ]},
  { label: 'Logic', types: [
    { value: 'counter_threshold', label: 'Counter Threshold' },
    { value: 'rule_fired',        label: 'Rule Fired' },
    { value: 'manual',            label: 'Manual (button / API)' },
  ]},
];

function triggerTypeOptions(selected) {
  return TRIGGER_GROUPS.map(g =>
    `<optgroup label="${escHtml(g.label)}">${
      g.types.map(t =>
        `<option value="${t.value}" ${selected === t.value ? 'selected' : ''}>${t.label}</option>`
      ).join('')
    }</optgroup>`
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
        <label>Device Event</label>
        <select disabled><option>Loading events from camera…</option></select>
      </div></div>`;
    }

    const matchIdx  = findCatalogMatch(t);
    const dataKeys  = matchIdx >= 0 ? vapixEventCatalog[matchIdx].dataKeys : [];

    /* Determine current condition type from saved fields */
    const condType  = t.cond_type || (t.value_key ? 'numeric' : (t.filter_key ? 'boolean' : (t.string_key ? 'string' : 'none')));
    const filterKey = t.filter_key || '';
    const filterVal = t.filter_value;
    const valueKey  = t.value_key  || (dataKeys[0] || '');
    const valueOp   = t.value_op   || 'gt';
    const valueThr  = t.value_threshold  !== undefined ? t.value_threshold  : '';
    const valueThr2 = t.value_threshold2 !== undefined ? t.value_threshold2 : '';
    const valueHold = t.value_hold_secs || 0;
    const stringKey   = t.string_key   || (dataKeys[0] || '');
    const stringValue = t.string_value || '';

    const condRow = dataKeys.length ? `
    <div class="form-row">
      <div class="form-group">
        <label>Value Condition <span style="opacity:.6">(optional — only fire when…)</span></label>
        <select data-k="cond_type" onchange="rerenderTrigger(this)" style="margin-bottom:8px">
          <option value="none"    ${condType==='none'    ?'selected':''}>Fire on every event</option>
          <option value="boolean" ${condType==='boolean' ?'selected':''}>Boolean match (true / false)</option>
          <option value="numeric" ${condType==='numeric' ?'selected':''}>Numeric threshold</option>
          <option value="string"  ${condType==='string'  ?'selected':''}>String match</option>
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
        ${condType === 'string' ? `
        <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
          <select data-k="string_key">
            <option value="">— pick field —</option>
            ${dataKeys.map(k =>
              `<option value="${escHtml(k)}" ${stringKey===k?'selected':''}>${escHtml(k)}</option>`
            ).join('')}
          </select>
          <span style="opacity:.7;font-size:12px">contains</span>
          <input type="text" data-k="string_value"
                 value="${escHtml(stringValue)}" placeholder="match substring">
        </div>` : ''}
        ${condType === 'numeric' ? `
        <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
          <select data-k="value_key">
            ${dataKeys.map(k =>
              `<option value="${escHtml(k)}" ${valueKey===k?'selected':''}>${escHtml(k)}</option>`
            ).join('')}
          </select>
          <select data-k="value_op" onchange="rerenderTrigger(this)">
            <option value="gt"      ${valueOp==='gt'      ?'selected':''}>is above</option>
            <option value="lt"      ${valueOp==='lt'      ?'selected':''}>is below</option>
            <option value="gte"     ${valueOp==='gte'     ?'selected':''}>is above or equal to</option>
            <option value="lte"     ${valueOp==='lte'     ?'selected':''}>is below or equal to</option>
            <option value="eq"      ${valueOp==='eq'      ?'selected':''}>equals</option>
            <option value="between" ${valueOp==='between' ?'selected':''}>is between</option>
          </select>
          ${valueOp === 'between' ? `
          <input type="number" data-k="value_threshold" step="any"
                 value="${escHtml(String(valueThr))}" placeholder="min"
                 style="width:80px">
          <span style="opacity:.7;font-size:12px">and</span>
          <input type="number" data-k="value_threshold2" step="any"
                 value="${escHtml(String(valueThr2))}" placeholder="max"
                 style="width:80px">
          ` : `
          <input type="number" data-k="value_threshold" step="any"
                 value="${escHtml(String(valueThr))}" placeholder="0"
                 style="width:90px">
          `}
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
        <label>Device Event</label>
        <select onchange="applyVapixEvent(${rowIdx}, this.value)">
          <option value="-1" ${matchIdx < 0 ? 'selected':''}>— Choose an event —</option>
          ${vapixEventCatalog.map((ev, i) =>
            `<option value="${i}" ${i===matchIdx?'selected':''}>${escHtml(ev.label)}</option>`
          ).join('')}
        </select>
        ${matchIdx < 0 && (t.topic0 || t.topic1)
          ? `<div class="form-hint" style="color:var(--text-muted)">Saved event not found in this device's catalog — it may still work if the device supports it.</div>`
          : `<div class="form-hint">Choose which device event fires this rule.</div>`}
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
        <div class="form-hint">Camera credentials (viewer or above) are required to call this URL. The token additionally identifies which rule to fire. Optionally send a JSON body — values are available as <code>{{trigger.KEY}}</code> in action templates.</div>
      </div>
    </div>`;
  }
  if (type === 'schedule') return `
    <div class="form-row">
      <div class="form-group">
        <label>Schedule Type</label>
        <select data-k="schedule_type" onchange="rerenderTrigger(this)">
          <option value="daily_time"    ${(t.schedule_type||'daily_time')==='daily_time'    ? 'selected' : ''}>Daily Time</option>
          <option value="interval"      ${t.schedule_type==='interval'      ? 'selected' : ''}>Interval</option>
          <option value="cron"          ${t.schedule_type==='cron'          ? 'selected' : ''}>Cron Expression</option>
          <option value="astronomical"  ${t.schedule_type==='astronomical'  ? 'selected' : ''}>Sunrise / Sunset</option>
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
    </div>` : ''}
    ${t.schedule_type === 'astronomical' ? `
    <div class="form-row">
      <div class="form-group">
        <label>Event</label>
        <select data-k="event" id="astro-event-${rowIdx}" onchange="refreshAstroTriggerPreview(${rowIdx})">
          <option value="sunrise"    ${(t.event||'sunrise')==='sunrise'    ? 'selected' : ''}>Sunrise</option>
          <option value="sunset"     ${t.event==='sunset'     ? 'selected' : ''}>Sunset</option>
          <option value="solar_noon" ${t.event==='solar_noon' ? 'selected' : ''}>Solar Noon</option>
          <option value="dawn"       ${t.event==='dawn'       ? 'selected' : ''}>Civil Dawn (−6°)</option>
          <option value="dusk"       ${t.event==='dusk'       ? 'selected' : ''}>Civil Dusk (−6°)</option>
        </select>
      </div>
      <div class="form-group">
        <label>Offset (minutes, + = later)</label>
        <input type="number" data-k="offset_minutes" id="astro-offset-${rowIdx}"
               value="${t.offset_minutes || 0}" placeholder="0"
               oninput="refreshAstroTriggerPreview(${rowIdx})">
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Latitude <span style="color:var(--text-muted);font-weight:400;">(decimal degrees)</span></label>
        <input type="number" step="0.0001" data-k="latitude" id="astro-lat-${rowIdx}"
               value="${t.latitude !== undefined ? t.latitude : engineLat}" placeholder="e.g. 59.3293"
               oninput="refreshAstroTriggerPreview(${rowIdx})">
        <div class="form-hint">North is positive (e.g. 59.33), South is negative (e.g. −33.87)</div>
      </div>
      <div class="form-group">
        <label>Longitude <span style="color:var(--text-muted);font-weight:400;">(decimal degrees)</span></label>
        <input type="number" step="0.0001" data-k="longitude" id="astro-lon-${rowIdx}"
               value="${t.longitude !== undefined ? t.longitude : engineLon}" placeholder="e.g. 18.0686"
               oninput="refreshAstroTriggerPreview(${rowIdx})">
        <div class="form-hint">East is positive (e.g. 18.07), West is negative (e.g. −73.94)</div>
      </div>
    </div>
    <div id="astro-preview-${rowIdx}"></div>` : ''}`;
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
      <div class="form-group" style="flex:0 0 140px;">
        <label>Hold duration (s)</label>
        <input type="number" data-k="hold_secs" min="0" value="${t.hold_secs || 0}">
        <div class="form-hint">Fire only if state holds for this many seconds (0 = immediate)</div>
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
        <label>Rule (leave empty to match any rule firing)</label>
        <select data-k="rule_id">
          <option value="" ${!t.rule_id ? 'selected' : ''}>— Any rule —</option>
          ${allRules.map(r => `<option value="${r.id}" ${t.rule_id === r.id ? 'selected' : ''}>${escHtml(r.name)}</option>`).join('')}
        </select>
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
  if (type === 'aoa_scenario') {
    let scenarioControl;
    if (aoaScenarios === null) {
      scenarioControl = `<input type="number" data-k="scenario_id" value="${t.scenario_id || 1}" min="1" placeholder="Loading…">`;
    } else if (!aoaScenarios.length) {
      scenarioControl = `<input type="number" data-k="scenario_id" value="${t.scenario_id || 1}" min="1"><div class="form-hint">No scenarios found — enter the scenario number manually (1-based).</div>`;
    } else {
      const opts = aoaScenarios.map(s => {
        const sel = (t.scenario_id !== undefined ? parseInt(t.scenario_id) === s.id : s.id === 1) ? 'selected' : '';
        return `<option value="${s.id}" ${sel}>${s.id}: ${escHtml(s.name || s.type || 'Scenario ' + s.id)}</option>`;
      }).join('');
      scenarioControl = `<select data-k="scenario_id">${opts}</select>`;
    }
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Scenario</label>
        ${scenarioControl}
        <div class="form-hint">Object Analytics scenario to monitor. Fires when the scenario generates an event.</div>
      </div>
      <div class="form-group">
        <label>Object Class <span style="font-weight:normal;opacity:.7">(optional filter)</span></label>
        <select data-k="object_class">
          <option value="any"          ${(!t.object_class||t.object_class==='any')     ?'selected':''}>Any</option>
          <option value="human"        ${t.object_class==='human'       ?'selected':''}>Human</option>
          <option value="car"          ${t.object_class==='car'         ?'selected':''}>Car</option>
          <option value="truck"        ${t.object_class==='truck'       ?'selected':''}>Truck</option>
          <option value="bus"          ${t.object_class==='bus'         ?'selected':''}>Bus</option>
          <option value="bike"         ${t.object_class==='bike'        ?'selected':''}>Bike</option>
          <option value="otherVehicle" ${t.object_class==='otherVehicle'?'selected':''}>Other Vehicle</option>
        </select>
        <div class="form-hint">Only fire when this object class is detected. Leave as "Any" for all classes.</div>
      </div>
    </div>`;
  }
  if (type === 'manual') return `
    <div class="form-row">
      <div class="form-group">
        <div class="form-hint" style="color:var(--text-muted);">This rule has no automatic trigger. Use the <strong>Fire</strong> button in the rule list to run it manually, or call <code>POST /local/acap_event_engine/fire</code> with <code>{"id":"&lt;rule_id&gt;"}</code>.</div>
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
  /* Initialise solar previews for any astronomical triggers */
  triggerRows.forEach((t, i) => {
    if ((t.type === 'schedule' || t.schedule_type === 'astronomical') &&
        t.schedule_type === 'astronomical')
      refreshAstroTriggerPreview(i);
  });
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
const CONDITION_GROUPS = [
  { label: 'Time', types: [
    { value: 'time_window',       label: 'Time Window' },
    { value: 'day_night',         label: 'Day / Night' },
  ]},
  { label: 'Device State', types: [
    { value: 'vapix_event_state', label: 'Device Event State' },
    { value: 'io_state',          label: 'I/O State' },
  ]},
  { label: 'Data', types: [
    { value: 'counter',           label: 'Counter Compare' },
    { value: 'variable_compare',  label: 'Variable Compare' },
    { value: 'aoa_occupancy',     label: 'AOA Occupancy' },
  ]},
  { label: 'External', types: [
    { value: 'http_check',        label: 'HTTP Check' },
  ]},
];

function conditionTypeOptions(selected) {
  return CONDITION_GROUPS.map(g =>
    `<optgroup label="${escHtml(g.label)}">${
      g.types.map(t =>
        `<option value="${t.value}" ${selected === t.value ? 'selected' : ''}>${t.label}</option>`
      ).join('')
    }</optgroup>`
  ).join('');
}

function vapixCatalogTopicPath(ev) {
  const parts = [];
  ['topic0','topic1','topic2','topic3'].forEach(k => {
    if (ev.topics && ev.topics[k]) {
      const entries = Object.entries(ev.topics[k]);
      if (entries.length > 0) {
        const [ns, val] = entries[0];
        if (val) parts.push(ns ? `${ns}:${val}` : val);
      }
    }
  });
  return parts.join('/');
}

function applyVapixEventState(rowIdx, idxStr) {
  const idx = parseInt(idxStr);
  if (idx < 0 || !vapixEventCatalog) return;
  const ev = vapixEventCatalog[idx];
  if (!ev) return;
  const path = vapixCatalogTopicPath(ev);
  const row = document.getElementById('crow-' + rowIdx);
  const data = { type: 'vapix_event_state' };
  if (row) row.querySelectorAll('[data-k]').forEach(inp => {
    data[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
  });
  data.event_key = path;
  if (ev.dataKeys.length === 1) data.data_key = ev.dataKeys[0];
  conditionRows[rowIdx] = data;
  renderConditionList();
}

function conditionFields(c, rowIdx) {
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
        <div class="form-hint">Tip: use <b>system.armed</b> = "true"/"false" for arm/disarm patterns</div>
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
        <div class="form-hint">Simple substring match in the response body</div>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>JSON Path (optional)</label>
        <input type="text" data-k="json_path" value="${escHtml(c.json_path || '')}" placeholder="status.value">
        <div class="form-hint">Dot-notation path into JSON response, e.g. <b>data.temperature</b></div>
      </div>
      <div class="form-group">
        <label>JSON Expected Value</label>
        <input type="text" data-k="json_expected" value="${escHtml(c.json_expected || '')}" placeholder="ok">
        <div class="form-hint">Value at the JSON path must equal this (string comparison)</div>
      </div>
    </div>
    <div class="form-row"><div class="form-group">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" data-k="allow_insecure" ${c.allow_insecure ? 'checked' : ''}>
        Allow untrusted certificate (skip SSL verification)
      </label>
      <div class="form-hint">Enable when the target URL uses a self-signed or expired certificate.</div>
    </div></div>`;
  if (type === 'aoa_occupancy') {
    let scenarioControl;
    if (aoaScenarios === null) {
      scenarioControl = `<input type="number" data-k="scenario_id" value="${c.scenario_id || 1}" min="1" placeholder="Loading…">`;
    } else if (!aoaScenarios.length) {
      scenarioControl = `<input type="number" data-k="scenario_id" value="${c.scenario_id || 1}" min="1">`;
    } else {
      const opts = aoaScenarios.map(s => {
        const sel = (c.scenario_id !== undefined ? parseInt(c.scenario_id) === s.id : s.id === 1) ? 'selected' : '';
        return `<option value="${s.id}" ${sel}>${s.id}: ${escHtml(s.name || s.type || 'Scenario ' + s.id)}</option>`;
      }).join('');
      scenarioControl = `<select data-k="scenario_id">${opts}</select>`;
    }
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Scenario</label>
        ${scenarioControl}
      </div>
      <div class="form-group">
        <label>Object Class</label>
        <select data-k="object_class">
          <option value="any"          ${(!c.object_class||c.object_class==='any')     ?'selected':''}>Any (total)</option>
          <option value="human"        ${c.object_class==='human'       ?'selected':''}>Human</option>
          <option value="car"          ${c.object_class==='car'         ?'selected':''}>Car</option>
          <option value="truck"        ${c.object_class==='truck'       ?'selected':''}>Truck</option>
          <option value="bus"          ${c.object_class==='bus'         ?'selected':''}>Bus</option>
          <option value="bike"         ${c.object_class==='bike'        ?'selected':''}>Bike</option>
          <option value="otherVehicle" ${c.object_class==='otherVehicle'?'selected':''}>Other Vehicle</option>
        </select>
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
        <label>Count</label>
        <input type="number" data-k="value" value="${c.value !== undefined ? c.value : 0}" min="0">
        <div class="form-hint">Current occupancy count must satisfy the condition to pass. This polls AOA on each rule evaluation.</div>
      </div>
    </div>`;
  }
  if (type === 'day_night') {
    const lat = c.lat !== undefined && c.lat !== '' ? parseFloat(c.lat) : engineLat;
    const lon = c.lon !== undefined && c.lon !== '' ? parseFloat(c.lon) : engineLon;
    const rise = calcSolarEvent(lat, lon, 'sunrise', 0);
    const set  = calcSolarEvent(lat, lon, 'sunset', 0);
    const solarHint = rise && set
      ? `Today: sunrise ${rise}, sunset ${set}`
      : (lat === 0 && lon === 0 ? 'Set latitude/longitude in Location' : 'No sunrise/sunset today (polar)');
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Pass when it is</label>
        <select data-k="state">
          <option value="day"   ${(c.state||'day')==='day'   ? 'selected' : ''}>Daytime (after sunrise)</option>
          <option value="night" ${c.state==='night' ? 'selected' : ''}>Nighttime (after sunset)</option>
        </select>
        <div class="form-hint">${solarHint}</div>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Latitude <span style="color:var(--text-muted);font-weight:400;">(optional override)</span></label>
        <input type="number" step="any" data-k="lat" value="${c.lat !== undefined ? c.lat : ''}" placeholder="From settings (${engineLat})">
      </div>
      <div class="form-group">
        <label>Longitude <span style="color:var(--text-muted);font-weight:400;">(optional override)</span></label>
        <input type="number" step="any" data-k="lon" value="${c.lon !== undefined ? c.lon : ''}" placeholder="From settings (${engineLon})">
      </div>
    </div>`;
  }
  if (type === 'vapix_event_state') {
    /* Build catalog quick-select if available */
    const catalogOpts = vapixEventCatalog && vapixEventCatalog.length
      ? vapixEventCatalog.map((ev, i) => {
          const path = vapixCatalogTopicPath(ev);
          const sel = c.event_key && path && path.includes(c.event_key) ? 'selected' : '';
          return `<option value="${i}" ${sel}>${escHtml(ev.label)}</option>`;
        }).join('')
      : '';
    /* Find data keys for currently matched catalog entry */
    const matchIdx = vapixEventCatalog
      ? vapixEventCatalog.findIndex(ev => { const p = vapixCatalogTopicPath(ev); return c.event_key && p && p.includes(c.event_key); })
      : -1;
    const dataKeys = matchIdx >= 0 ? vapixEventCatalog[matchIdx].dataKeys : [];
    const dataKeyCtrl = dataKeys.length
      ? `<select data-k="data_key">${dataKeys.map(k => `<option value="${escHtml(k)}" ${c.data_key===k?'selected':''}>${escHtml(k)}</option>`).join('')}</select>`
      : `<input type="text" data-k="data_key" value="${escHtml(c.data_key || '')}" placeholder="active">`;
    return `
    ${catalogOpts ? `
    <div class="form-row">
      <div class="form-group">
        <label>Event <span style="color:var(--text-muted);font-weight:400;">(pick to auto-fill fields below)</span></label>
        <select onchange="applyVapixEventState(${rowIdx}, this.value)">
          <option value="-1" ${matchIdx < 0 ? 'selected' : ''}>— custom / enter manually —</option>
          ${catalogOpts}
        </select>
      </div>
    </div>` : ''}
    <div class="form-row">
      <div class="form-group">
        <label>Event Topic (partial match)</label>
        <input type="text" data-k="event_key" value="${escHtml(c.event_key || '')}" placeholder="tns1:Device/tnsaxis:IO/VirtualInput">
        <div class="form-hint">Substring matched against the event topic path returned by the camera</div>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Data Key</label>
        ${dataKeyCtrl}
      </div>
      <div class="form-group">
        <label>Expected Value</label>
        <input type="text" data-k="expected" value="${escHtml(c.expected || '')}" placeholder="1 or true">
      </div>
    </div>`;
  }
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
      <div class="tca-fields">${conditionFields(c, i)}</div>
    </div>
  `).join('');
}

function addConditionRow() { conditionRows.push({ type: 'time_window' }); renderConditionList(); }
function removeConditionRow(i) { conditionRows.splice(i, 1); renderConditionList(); }
function changeConditionType(i, type) { conditionRows[i] = { type }; renderConditionList(); }

/* ===== Action rows ===== */
const ACTION_GROUPS = [
  { label: 'Notifications', types: [
    { value: 'http_request',      label: 'HTTP Request' },
    { value: 'slack_webhook',     label: 'Slack' },
    { value: 'teams_webhook',     label: 'Microsoft Teams' },
    { value: 'telegram',          label: 'Telegram' },
    { value: 'email',             label: 'Email (SMTP)' },
    { value: 'mqtt_publish',      label: 'MQTT Publish' },
    { value: 'ftp_upload',        label: 'FTP Upload' },
    { value: 'snapshot_upload',   label: 'Snapshot Upload' },
    { value: 'influxdb_write',    label: 'InfluxDB Write' },
    { value: 'send_syslog',       label: 'Send Syslog' },
  ]},
  { label: 'Camera', types: [
    { value: 'recording',         label: 'Recording' },
    { value: 'ptz_preset',        label: 'PTZ Preset' },
    { value: 'guard_tour',        label: 'Guard Tour' },
    { value: 'overlay_text',      label: 'Overlay Text' },
    { value: 'ir_cut_filter',     label: 'IR Cut Filter' },
    { value: 'privacy_mask',      label: 'Privacy Mask' },
    { value: 'wiper',             label: 'Wiper' },
    { value: 'light_control',     label: 'Light Control' },
    { value: 'audio_clip',        label: 'Audio Clip' },
    { value: 'siren_light',       label: 'Siren / Light' },
  ]},
  { label: 'I/O', types: [
    { value: 'io_output',         label: 'I/O Output' },
  ]},
  { label: 'Logic', types: [
    { value: 'delay',             label: 'Delay' },
    { value: 'set_variable',      label: 'Set Variable' },
    { value: 'increment_counter', label: 'Increment Counter' },
    { value: 'run_rule',          label: 'Run Rule' },
    { value: 'digest',            label: 'Notification Digest' },
  ]},
  { label: 'Advanced', types: [
    { value: 'fire_vapix_event',  label: 'Fire ACAP Event' },
    { value: 'vapix_query',       label: 'Device Event Query' },
    { value: 'set_device_param',  label: 'Set Device Parameter' },
    { value: 'acap_control',      label: 'ACAP Control' },
    { value: 'aoa_get_counts',    label: 'AOA Get Counts' },
  ]},
];

function actionTypeOptions(selected) {
  return ACTION_GROUPS.map(g =>
    `<optgroup label="${escHtml(g.label)}">${
      g.types.map(t =>
        `<option value="${t.value}" ${selected === t.value ? 'selected' : ''}>${t.label}</option>`
      ).join('')
    }</optgroup>`
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
      tokens.push('{{trigger.counter_name}}', '{{trigger.counter_value}}');
    } else if (t.type === 'io_input') {
      tokens.push('{{trigger.port}}', '{{trigger.state}}');
    } else if (t.type === 'aoa_scenario') {
      tokens.push('{{trigger.scenario_id}}', '{{trigger.object_class}}');
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
  /* Check if any action in the current form has attach_snapshot enabled */
  const hasSnapshot = actionRows.some(a =>
    ['http_request', 'email', 'mqtt_publish', 'slack_webhook', 'teams_webhook', 'telegram'].includes(a.type) && a.attach_snapshot
  );
  const groups = [
    { label: 'Time',    tokens: ['{{timestamp}}', '{{date}}', '{{time}}'] },
    { label: 'Camera',  tokens: ['{{camera.serial}}', '{{camera.model}}', '{{camera.ip}}',
                                 ...(hasSnapshot ? ['{{trigger.snapshot_base64}}'] : [])] },
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
  const header = `<div style="padding:6px 12px 4px;font-size:11px;color:var(--text-dim);border-bottom:1px solid var(--border);margin-bottom:2px;">Click to insert into field above</div>`;
  panel.innerHTML = header + groups.map(g => label(g.label) + g.tokens.map(row).join('')).join('');
  document.body.appendChild(panel);

  /* Position inside viewport: clamp horizontally and flip upward near bottom edge. */
  const rect = btn.getBoundingClientRect();
  const margin = 8;
  const gap = 4;

  const panelW = Math.max(200, panel.offsetWidth || 210);
  let left = rect.left;
  if (left + panelW > window.innerWidth - margin) left = rect.right - panelW;
  left = Math.max(margin, Math.min(left, window.innerWidth - panelW - margin));

  const spaceBelow = window.innerHeight - rect.bottom - margin;
  const spaceAbove = rect.top - margin;
  const preferBelow = spaceBelow >= 140 || spaceBelow >= spaceAbove;
  const available = Math.max(120, Math.min(240, preferBelow ? spaceBelow : spaceAbove));
  panel.style.maxHeight = available + 'px';

  const panelH = panel.offsetHeight;
  let top;
  if (preferBelow) {
    top = rect.bottom + gap;
    if (top + panelH > window.innerHeight - margin)
      top = Math.max(margin, window.innerHeight - panelH - margin);
  } else {
    top = rect.top - panelH - gap;
    if (top < margin)
      top = margin;
  }

  panel.style.left = left + 'px';
  panel.style.top = top + 'px';

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
            title="Insert a dynamic value placeholder — e.g. {{timestamp}} or {{trigger.Temperature}}"
            style="font-size:11px;padding:2px 8px;font-family:monospace;">&#123;&#125; Insert variable</button>
    <span style="margin-left:6px;opacity:0.6;font-size:11px;">— dynamically replaced when the rule fires</span>
  </div>`;
}

function renderOnFailureFields(a) {
  /* on_failure is stored as array; for UI we support one fallback action.
   * Also handle the flat form fields used during re-render after collectRows. */
  const fb = (a.on_failure && a.on_failure[0]) || {};
  const fbType = a.on_failure_type || fb.type || '';
  return `
  <div class="form-row" style="border-top:1px solid var(--border);margin-top:8px;padding-top:10px;">
    <div class="form-group">
      <label>On Failure — Fallback action</label>
      <select data-k="on_failure_type" onchange="rerenderAction(this)">
        <option value=""              ${!fbType              ? 'selected' : ''}>None</option>
        <option value="send_syslog"  ${fbType==='send_syslog'  ? 'selected' : ''}>Log message</option>
        <option value="mqtt_publish" ${fbType==='mqtt_publish' ? 'selected' : ''}>MQTT publish</option>
        <option value="http_request" ${fbType==='http_request' ? 'selected' : ''}>HTTP request</option>
      </select>
      <div class="form-hint">Executed when the HTTP request fails (non-2xx or network error)</div>
    </div>
  </div>
  ${fbType === 'send_syslog' ? `
  <div class="form-row"><div class="form-group">
    <label>Log Message</label>
    <input type="text" data-k="on_failure_message" value="${escHtml(a.on_failure_message || fb.message || 'HTTP request failed')}" placeholder="HTTP request failed">
  </div></div>` : ''}
  ${fbType === 'mqtt_publish' ? `
  <div class="form-row">
    <div class="form-group">
      <label>Fallback MQTT Topic</label>
      <input type="text" data-k="on_failure_topic" value="${escHtml(a.on_failure_topic || fb.topic || '')}">
    </div>
    <div class="form-group">
      <label>Fallback Payload</label>
      <input type="text" data-k="on_failure_payload" value="${escHtml(a.on_failure_payload || fb.payload || 'http_request_failed')}">
    </div>
  </div>` : ''}
  ${fbType === 'http_request' ? `
  <div class="form-row"><div class="form-group">
    <label>Fallback URL (GET)</label>
    <input type="text" data-k="on_failure_url" value="${escHtml(a.on_failure_url || fb.url || '')}" placeholder="https://example.com/alert">
  </div></div>` : ''}`;
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
        <label>Username <span style="color:var(--text-muted);font-weight:400;">(optional)</span></label>
        <input type="text" data-k="username" value="${escHtml(a.username || '')}" autocomplete="off">
      </div>
      <div class="form-group">
        <label>Password <span style="color:var(--text-muted);font-weight:400;">(optional)</span></label>
        <input type="password" data-k="password" value="${escHtml(a.password || '')}" placeholder="(unchanged)" autocomplete="new-password">
      </div>
    </div>
    <div class="form-row">
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
    </div>
    <div class="form-row"><div class="form-group">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" data-k="attach_snapshot" ${a.attach_snapshot ? 'checked' : ''}>
        Attach camera snapshot
      </label>
      <div class="form-hint">Fetches a JPEG from the camera and injects it as <b>{{trigger.snapshot_base64}}</b> — use in the body to send the image as base64.</div>
    </div></div>
    <div class="form-row"><div class="form-group">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" data-k="allow_insecure" ${a.allow_insecure ? 'checked' : ''}>
        Allow untrusted certificate (skip SSL verification)
      </label>
      <div class="form-hint">Enable when the target URL uses a self-signed or expired certificate.</div>
    </div></div>
    ${renderOnFailureFields(a)}`;
  if (type === 'recording') {
    const recStart = (a.operation || 'start') === 'start';
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Operation</label>
        <select data-k="operation" onchange="rerenderAction(this)">
          <option value="start" ${recStart ? 'selected' : ''}>Start Recording</option>
          <option value="stop"  ${!recStart ? 'selected' : ''}>Stop Recording</option>
        </select>
      </div>
      <div class="form-group">
        <label>Max Duration (s, 0=unlimited)</label>
        <input type="number" data-k="duration" min="0" value="${a.duration || 0}">
      </div>
    </div>
    ${recStart ? `
    <div class="form-row">
      <div class="form-group">
        <label>Stream Profile</label>
        <input type="text" data-k="profile" value="${escHtml(a.profile || '')}" placeholder="Quality">
        <div class="form-hint">Name of the stream profile configured on the camera. Defaults to Quality if left blank.</div>
      </div>
    </div>
    <div class="form-row"><div class="form-group">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" data-k="while_active" ${a.while_active ? 'checked' : ''}>
        Run while active - automatically stop when the triggering condition clears
      </label>
    </div></div>` : ''}`;
  }
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
        <input type="number" data-k="duration" min="0" value="${a.duration || 0}" onchange="rerenderAction(this)">
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
    </div>
    ${!(parseInt(a.duration) > 0) ? `
    <div class="form-row"><div class="form-group">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" data-k="while_active" ${a.while_active ? 'checked' : ''}>
        Run while active - automatically clear overlay when the triggering condition clears
      </label>
    </div></div>` : ''}`;
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
  if (type === 'io_output') {
    const ioDur = parseInt(a.duration) || 0;
    return `
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
        <label>Duration (s, 0=permanent)</label>
        <input type="number" data-k="duration" min="0" value="${ioDur}" onchange="rerenderAction(this)">
      </div>
    </div>
    ${ioDur === 0 ? `
    <div class="form-row"><div class="form-group">
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
        <input type="checkbox" data-k="while_active" ${a.while_active ? 'checked' : ''}>
        Run while active - automatically reset port when the triggering condition clears
      </label>
    </div></div>` : ''}`;
  }
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
  if (type === 'siren_light') {
    let profileControl;
    if (sirenProfiles === null) {
      profileControl = `<input type="text" data-k="profile" value="${escHtml(a.profile || '')}" placeholder="Loading profiles..." disabled>`;
    } else if (!sirenProfiles.length) {
      profileControl = `<input type="text" data-k="profile" value="${escHtml(a.profile || '')}" placeholder="Profile name (e.g. Green)">`;
    } else {
      let opts = `<option value="">— select profile —</option>`;
      for (const p of sirenProfiles) {
        const sel = a.profile === p.name ? 'selected' : '';
        opts += `<option value="${escHtml(p.name)}" ${sel}>${escHtml(p.label)}</option>`;
      }
      profileControl = `<select data-k="profile">${opts}</select>`;
    }
    const isStart = (a.signal_action || 'start') === 'start';
    return `
    <div class="form-row">
      <div class="form-group" style="flex:0 0 120px;">
        <label>Action</label>
        <select data-k="signal_action" onchange="rerenderAction(this)">
          <option value="start" ${isStart ? 'selected':''}>Start</option>
          <option value="stop"  ${!isStart ? 'selected':''}>Stop</option>
        </select>
      </div>
      <div class="form-group">
        <label>Profile</label>${profileControl}
        <div class="form-hint">Profile names are configured in the camera's Siren and Light settings.</div>
      </div>
    </div>
    ${isStart ? `
    <div class="form-row">
      <div class="form-group">
        <label style="display:flex;align-items:center;gap:8px;cursor:pointer">
          <input type="checkbox" data-k="while_active" ${a.while_active ? 'checked' : ''}>
          Run while active - automatically stop when the triggering condition clears
        </label>
      </div>
    </div>` : ''}`;
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
        <label>Device Event to Query</label>
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
  if (type === 'fire_vapix_event') {
    const events = Array.isArray(acapEvents) ? acapEvents : [];
    const selId = a.event_id || (events.length > 0 ? events[0].id : '');
    const selEvent = events.find(e => e.id === selId) || null;
    const isStateful = selEvent ? !!selEvent.state : false;
    const isAdding = a._adding === 'true';
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Event</label>
        <div style="display:flex;gap:6px;align-items:center;">
          <select data-k="event_id" onchange="rerenderAction(this)" style="flex:1;">
            ${events.length === 0 ? '<option value="">— No events declared —</option>' : ''}
            ${events.map(e => `<option value="${escHtml(e.id)}" ${e.id === selId ? 'selected' : ''}>${escHtml(e.name || e.id)}${e.state ? ' (stateful)' : ''}</option>`).join('')}
          </select>
          <button type="button" class="btn btn-ghost btn-sm" onclick="acapEventStartAdd(this)" title="Create a new custom ACAP event">＋ New</button>
        </div>
        ${selEvent ? `<div class="form-hint">${escHtml(selEvent.name || selEvent.id)} · ${selEvent.system ? 'System event' : 'Custom event'} · ${selEvent.state ? 'Stateful (High/Low)' : 'Stateless (pulse)'}</div>` : '<div class="form-hint">No events available — use ＋ New to create one.</div>'}
        ${selEvent && !selEvent.system ? `<button type="button" class="btn btn-ghost btn-sm" style="margin-top:4px;color:var(--danger,#e05252);" onclick="acapEventDelete('${escHtml(selId)}')">Remove Event</button>` : ''}
      </div>
    </div>
    ${isStateful ? `
    <div class="form-row">
      <div class="form-group" style="flex:0 0 140px;">
        <label>State</label>
        <select data-k="state">
          <option value="true"  ${a.state === true || a.state === 'true'  ? 'selected' : ''}>High (on)</option>
          <option value="false" ${a.state === false || a.state === 'false' ? 'selected' : ''}>Low (off)</option>
        </select>
      </div>
    </div>` : ''}
    ${isAdding ? `
    <div class="form-row" style="background:var(--bg-2,#1e2230);border-radius:6px;padding:12px;gap:8px;flex-wrap:wrap;">
      <div class="form-group" style="min-width:140px;">
        <label>Event ID</label>
        <input type="text" data-k="_new_id" value="${escHtml(a._new_id || '')}" placeholder="e.g. DoorOpenAlert" autocomplete="off">
        <div class="form-hint">Letters, digits and underscores only.</div>
      </div>
      <div class="form-group" style="min-width:160px;">
        <label>Display Name</label>
        <input type="text" data-k="_new_name" value="${escHtml(a._new_name !== undefined ? a._new_name : 'Event Engine: ')}" placeholder="e.g. Event Engine: Door Open" autocomplete="off">
      </div>
      <div class="form-group" style="flex:0 0 160px;">
        <label>Type</label>
        <select data-k="_new_stateful">
          <option value=""     ${!a._new_stateful ? 'selected' : ''}>Stateless (pulse)</option>
          <option value="true" ${a._new_stateful === 'true' ? 'selected' : ''}>Stateful (High/Low)</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 auto;align-self:flex-end;display:flex;gap:6px;">
        <button type="button" class="btn btn-primary btn-sm" onclick="acapEventSaveNew(this)">Create</button>
        <button type="button" class="btn btn-ghost btn-sm" onclick="acapEventCancelAdd(this)">Cancel</button>
      </div>
    </div>` : ''}
    <div class="form-hint" style="margin-top:4px;">ACAP events appear in the camera's built-in Action Rules and are visible to other ACAP applications on this device.</div>`
  }
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
      <div class="form-group" style="flex:0 0 220px;">
        <label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:18px;">
          <input type="checkbox" data-k="attach_snapshot" ${a.attach_snapshot ? 'checked' : ''}>
          Include snapshot as {{trigger.snapshot_base64}}
        </label>
      </div>
    </div>`;
  if (type === 'slack_webhook') return `
    <div class="form-row"><div class="form-group">
      <label>Webhook URL</label>
      <input type="text" data-k="webhook_url" value="${escHtml(a.webhook_url || '')}" placeholder="https://hooks.slack.com/services/...">
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Message</label>
      <textarea data-k="message" placeholder="Motion detected on {{camera.name}} at {{timestamp}}">${escHtml(a.message || '')}</textarea>
      ${hint}
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:8px;">
        <input type="checkbox" data-k="attach_snapshot" ${a.attach_snapshot ? 'checked' : ''}>
        Include snapshot as {{trigger.snapshot_base64}}
      </label>
    </div></div>
    <div class="form-row">
      <div class="form-group">
        <label>Channel <span style="color:var(--text-muted);font-weight:400;">(optional override)</span></label>
        <input type="text" data-k="channel" value="${escHtml(a.channel || '')}" placeholder="#alerts">
      </div>
      <div class="form-group">
        <label>Username <span style="color:var(--text-muted);font-weight:400;">(optional)</span></label>
        <input type="text" data-k="username" value="${escHtml(a.username || '')}" placeholder="Camera Bot">
      </div>
    </div>`;
  if (type === 'teams_webhook') return `
    <div class="form-row"><div class="form-group">
      <label>Webhook URL</label>
      <input type="text" data-k="webhook_url" value="${escHtml(a.webhook_url || '')}" placeholder="https://...webhook.office.com/...">
      <div class="form-hint">Use a Power Automate / Workflows incoming webhook connector</div>
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Title <span style="color:var(--text-muted);font-weight:400;">(optional)</span></label>
      <input type="text" data-k="title" value="${escHtml(a.title || '')}" placeholder="Alert from {{camera.name}}">
      ${hint}
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Message</label>
      <textarea data-k="message" placeholder="Motion detected at {{timestamp}}">${escHtml(a.message || '')}</textarea>
      ${hint}
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:8px;">
        <input type="checkbox" data-k="attach_snapshot" ${a.attach_snapshot ? 'checked' : ''}>
        Include snapshot as {{trigger.snapshot_base64}}
      </label>
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Theme Colour <span style="color:var(--text-muted);font-weight:400;">(optional hex)</span></label>
      <input type="text" data-k="theme_color" value="${escHtml(a.theme_color || '')}" placeholder="FF0000" style="max-width:140px;">
    </div></div>`;
  if (type === 'influxdb_write') return `
    <div class="form-row">
      <div class="form-group">
        <label>InfluxDB URL</label>
        <input type="text" data-k="url" value="${escHtml(a.url || '')}" placeholder="http://influxdb:8086">
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>Version</label>
        <select data-k="version">
          <option value="v2" ${(a.version||'v2')==='v2' ? 'selected' : ''}>v2</option>
          <option value="v1" ${a.version==='v1' ? 'selected' : ''}>v1</option>
        </select>
      </div>
    </div>
    ${(a.version||'v2') === 'v1' ? `
    <div class="form-row">
      <div class="form-group"><label>Database</label>
        <input type="text" data-k="database" value="${escHtml(a.database || '')}">
      </div>
      <div class="form-group"><label>Username <span style="color:var(--text-muted);font-weight:400;">(optional)</span></label>
        <input type="text" data-k="username" value="${escHtml(a.username || '')}">
      </div>
      <div class="form-group"><label>Password <span style="color:var(--text-muted);font-weight:400;">(optional)</span></label>
        <input type="password" data-k="token" value="${escHtml(a.token || '')}" autocomplete="new-password">
      </div>
    </div>` : `
    <div class="form-row">
      <div class="form-group"><label>Organization</label>
        <input type="text" data-k="org" value="${escHtml(a.org || '')}">
      </div>
      <div class="form-group"><label>Bucket</label>
        <input type="text" data-k="bucket" value="${escHtml(a.bucket || '')}">
      </div>
    </div>
    <div class="form-row"><div class="form-group">
      <label>API Token</label>
      <input type="password" data-k="token" value="${escHtml(a.token || '')}" autocomplete="new-password">
    </div></div>`}
    <div class="form-row"><div class="form-group">
      <label>Measurement</label>
      <input type="text" data-k="measurement" value="${escHtml(a.measurement || '')}" placeholder="camera_events">
      ${hint}
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Tags <span style="color:var(--text-muted);font-weight:400;">(optional, comma-separated key=value)</span></label>
      <input type="text" data-k="tags" value="${escHtml(a.tags || '')}" placeholder="camera={{camera.serial}},location=entrance">
      ${hint}
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Fields <span style="color:var(--text-muted);font-weight:400;">(comma-separated key=value)</span></label>
      <input type="text" data-k="fields" value="${escHtml(a.fields || '')}" placeholder="co2={{trigger.CO2}},temperature={{trigger.Temperature}}">
      ${hint}
      <div class="form-hint">Numeric values sent as-is. Wrap strings in double quotes: <code>name="{{trigger.name}}"</code></div>
    </div></div>`;
  if (type === 'telegram') return `
    <div class="form-row">
      <div class="form-group">
        <label>Bot Token</label>
        <input type="password" data-k="bot_token" value="${escHtml(a.bot_token || '')}" placeholder="123456:ABC-DEF..." autocomplete="new-password">
      </div>
      <div class="form-group">
        <label>Chat ID</label>
        <input type="text" data-k="chat_id" value="${escHtml(a.chat_id || '')}" placeholder="-1001234567890">
      </div>
    </div>
    <div class="form-row"><div class="form-group">
      <label>Message</label>
      <textarea data-k="message" placeholder="Motion detected on {{camera.name}} at {{timestamp}}">${escHtml(a.message || '')}</textarea>
      ${hint}
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:8px;">
        <input type="checkbox" data-k="attach_snapshot" ${a.attach_snapshot ? 'checked' : ''}>
        Send camera snapshot as photo attachment
      </label>
      <div class="form-hint">Uses Telegram <code>sendPhoto</code> when enabled. Also exposes <b>{{trigger.snapshot_base64}}</b> in templates.</div>
    </div></div>
    <div class="form-row">
      <div class="form-group" style="flex:0 0 140px;">
        <label>Parse Mode</label>
        <select data-k="parse_mode">
          <option value=""         ${!a.parse_mode ? 'selected' : ''}>Plain text</option>
          <option value="Markdown" ${a.parse_mode==='Markdown' ? 'selected' : ''}>Markdown</option>
          <option value="HTML"     ${a.parse_mode==='HTML'     ? 'selected' : ''}>HTML</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 160px;">
        <label>Disable Link Preview</label>
        <input type="checkbox" data-k="disable_preview" ${a.disable_preview ? 'checked' : ''}>
      </div>
    </div>`;
  if (type === 'email') return `
    <div class="form-row"><div class="form-group">
      <label>To</label>
      <input type="text" data-k="to" value="${escHtml(a.to || '')}" placeholder="alerts@example.com">
      <div class="form-hint">Comma-separated for multiple recipients. SMTP server is configured in the Settings tab.</div>
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Subject</label>
      <input type="text" data-k="subject" value="${escHtml(a.subject || '')}" placeholder="Alert from {{camera.name}}">
      ${hint}
    </div></div>
    <div class="form-row"><div class="form-group">
      <label>Body</label>
      <textarea data-k="body" placeholder="Motion detected at {{timestamp}}&#10;Camera: {{camera.serial}}">${escHtml(a.body || '')}</textarea>
      ${hint}
      <label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:8px;">
        <input type="checkbox" data-k="attach_snapshot" ${a.attach_snapshot ? 'checked' : ''}>
        Attach camera snapshot (JPEG)
      </label>
      <div class="form-hint">Adds a <b>snapshot.jpg</b> attachment to the email and exposes <b>{{trigger.snapshot_base64}}</b> for template use.</div>
    </div></div>`;
  if (type === 'ftp_upload') return `
    <div class="form-row"><div class="form-group">
      <label>FTP URL</label>
      <input type="text" data-k="url" value="${escHtml(a.url || '')}" placeholder="ftp://server/cameras/{{camera.serial}}/{{date}}_{{time}}.jpg">
      ${hint}
      <div class="form-hint">Supports FTP and SFTP. Directories are created automatically. Use <code>sftp://</code> for SFTP.</div>
    </div></div>
    <div class="form-row">
      <div class="form-group">
        <label>Username</label>
        <input type="text" data-k="username" value="${escHtml(a.username || '')}">
      </div>
      <div class="form-group">
        <label>Password</label>
        <input type="password" data-k="password" value="${escHtml(a.password || '')}" autocomplete="new-password">
      </div>
    </div>`;
  if (type === 'digest') return `
    <div class="form-row">
      <div class="form-group">
        <label>Deliver via</label>
        <select data-k="deliver_via" onchange="rerenderAction(this)">
          <option value="slack"    ${(a.deliver_via||'slack')==='slack'    ? 'selected' : ''}>Slack</option>
          <option value="teams"    ${a.deliver_via==='teams'    ? 'selected' : ''}>Teams</option>
          <option value="telegram" ${a.deliver_via==='telegram' ? 'selected' : ''}>Telegram</option>
          <option value="email"    ${a.deliver_via==='email'    ? 'selected' : ''}>Email</option>
          <option value="mqtt"     ${a.deliver_via==='mqtt'     ? 'selected' : ''}>MQTT</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 140px;">
        <label>Flush every</label>
        <input type="number" data-k="interval" value="${a.interval || 300}" min="30" step="30"> <span style="color:var(--text-muted);">sec</span>
      </div>
    </div>
    ${(a.deliver_via||'slack') === 'slack' ? `
    <div class="form-row"><div class="form-group">
      <label>Webhook URL</label>
      <input type="text" data-k="webhook_url" value="${escHtml(a.webhook_url || '')}" placeholder="https://hooks.slack.com/services/...">
    </div></div>` : ''}
    ${a.deliver_via === 'teams' ? `
    <div class="form-row"><div class="form-group">
      <label>Webhook URL</label>
      <input type="text" data-k="webhook_url" value="${escHtml(a.webhook_url || '')}" placeholder="https://...webhook.office.com/...">
    </div></div>` : ''}
    ${a.deliver_via === 'telegram' ? `
    <div class="form-row">
      <div class="form-group"><label>Bot Token</label>
        <input type="password" data-k="bot_token" value="${escHtml(a.bot_token || '')}" autocomplete="new-password">
      </div>
      <div class="form-group"><label>Chat ID</label>
        <input type="text" data-k="chat_id" value="${escHtml(a.chat_id || '')}">
      </div>
    </div>` : ''}
    ${a.deliver_via === 'email' ? `
    <div class="form-row">
      <div class="form-group"><label>To</label><input type="text" data-k="to" value="${escHtml(a.to || '')}" placeholder="alerts@example.com">
        <div class="form-hint">SMTP server configured in the Settings tab</div>
      </div>
    </div>
    <div class="form-row"><div class="form-group">
      <label>Subject</label>
      <input type="text" data-k="subject" value="${escHtml(a.subject || '')}" placeholder="Digest from {{camera.name}}">
    </div></div>` : ''}
    ${a.deliver_via === 'mqtt' ? `
    <div class="form-row"><div class="form-group">
      <label>Topic</label>
      <input type="text" data-k="topic" value="${escHtml(a.topic || '')}" placeholder="cameras/{{camera.serial}}/digest">
    </div></div>` : ''}
    <div class="form-row"><div class="form-group">
      <label>Line Template <span style="color:var(--text-muted);font-weight:400;">(per event)</span></label>
      <input type="text" data-k="line" value="${escHtml(a.line || '')}" placeholder="{{timestamp}} — {{trigger_json}}">
      ${hint}
      <div class="form-hint">Each event adds one line. All lines are combined and sent as a single message at the flush interval.</div>
    </div></div>`;
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
  if (type === 'guard_tour') {
    const isStart = (a.operation || 'start') === 'start';
    let tourControl = '';
    if (isStart) {
      if (guardTours === null) {
        tourControl = `<input type="text" data-k="tour_id" value="${escHtml(a.tour_id || '')}" placeholder="Loading tours…" disabled>`;
      } else if (!guardTours.length) {
        tourControl = `<input type="text" data-k="tour_id" value="${escHtml(a.tour_id || '')}" placeholder="e.g. Guard Tour 1">`;
      } else {
        let opts = `<option value="">— select tour —</option>`;
        for (const gt of guardTours) {
          const sel = a.tour_id === gt.name ? 'selected' : '';
          opts += `<option value="${escHtml(gt.name)}" ${sel}>${escHtml(gt.name)}</option>`;
        }
        tourControl = `<select data-k="tour_id">${opts}</select>`;
      }
    }
    return `
    <div class="form-row">
      <div class="form-group" style="flex:0 0 120px;">
        <label>Operation</label>
        <select data-k="operation" onchange="rerenderAction(this)">
          <option value="start" ${isStart  ? 'selected':''}>Start</option>
          <option value="stop"  ${!isStart ? 'selected':''}>Stop</option>
        </select>
      </div>
      ${isStart ? `
      <div class="form-group">
        <label>Guard Tour</label>${tourControl}
      </div>` : ''}
      <div class="form-group" style="flex:0 0 100px;">
        <label>Channel</label>
        <input type="number" data-k="channel" value="${a.channel || 1}" min="1" max="8">
      </div>
    </div>`;
  }
  if (type === 'set_device_param') {
    const paramListId = `param-list-${Math.random().toString(36).slice(2)}`;
    const valListId   = `val-list-${Math.random().toString(36).slice(2)}`;
    const paramDl = deviceParams && deviceParams.length
      ? `<datalist id="${paramListId}">${deviceParams.map(p => `<option value="${escHtml(p)}">`).join('')}</datalist>`
      : '';
    const placeholder = deviceParams === null ? 'Loading params…' : 'e.g. root.ImageSource.I0.Sensor.Sharpness';
    return `${paramDl}<datalist class="param-val-list" id="${valListId}"></datalist>
    <div class="form-row">
      <div class="form-group" style="width:100%">
        <div style="background:rgba(255,160,0,0.12);border:1px solid rgba(255,160,0,0.5);border-radius:6px;padding:8px 12px;font-size:12px;">
          <strong>For expert users only.</strong> Only change these settings if you know what you are doing — incorrect values can disrupt camera operation.
        </div>
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Parameter</label>
        <input type="text" data-k="parameter" value="${escHtml(a.parameter || '')}" placeholder="${placeholder}" list="${paramListId}" ${deviceParams === null ? 'disabled' : ''}
               onblur="fetchParamValues(this)">
        <div class="form-hint">The <code>root.</code> prefix is added automatically if omitted. Tab out of this field to look up allowed values.</div>
      </div>
      <div class="form-group" style="flex:0 0 200px;">
        <label>Value</label>
        <input type="text" data-k="value" value="${escHtml(a.value || '')}" list="${valListId}">
        <div class="form-hint param-val-hint">${a.parameter ? 'Tab the parameter field to look up values.' : ''}</div>
      </div>
    </div>`;
  }
  if (type === 'snapshot_upload') return `
    <div class="form-row">
      <div class="form-group">
        <label>URL</label>
        <input type="text" data-k="url" value="${escHtml(a.url || '')}" placeholder="https://example.com/upload">
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>Method</label>
        <select data-k="method">
          <option value="POST" ${(a.method||'POST')==='POST' ? 'selected':''}>POST</option>
          <option value="PUT"  ${a.method==='PUT'           ? 'selected':''}>PUT</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 80px;">
        <label>Channel</label>
        <input type="number" data-k="channel" value="${a.channel || 1}" min="1" max="8">
      </div>
    </div>
    <div class="form-row">
      <div class="form-group" style="flex:0 0 200px;">
        <label>Username</label>
        <input type="text" data-k="username" value="${escHtml(a.username || '')}">
      </div>
      <div class="form-group" style="flex:0 0 200px;">
        <label>Password</label>
        <input type="password" data-k="password" value="${escHtml(a.password || '')}">
      </div>
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Extra Headers</label>
        <textarea data-k="headers" rows="2" placeholder="X-Token: abc123">${escHtml(a.headers || '')}</textarea>
      </div>
    </div>`;
  if (type === 'ir_cut_filter') return `
    <div class="form-row">
      <div class="form-group" style="flex:0 0 180px;">
        <label>Mode</label>
        <select data-k="mode">
          <option value="day"   ${a.mode==='day'             ? 'selected':''}>On (day — filter in)</option>
          <option value="night" ${a.mode==='night'           ? 'selected':''}>Off (night — filter out)</option>
          <option value="auto"  ${(a.mode||'auto')==='auto'  ? 'selected':''}>Auto</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>Channel</label>
        <input type="number" data-k="channel" value="${a.channel || 1}" min="1" max="8">
      </div>
    </div>`;
  if (type === 'privacy_mask') {
    let maskControl;
    if (privacyMasks === null) {
      maskControl = `<input type="text" data-k="mask_id" value="${escHtml(a.mask_id || '')}" placeholder="Loading masks…" disabled>`;
    } else if (!privacyMasks.length) {
      maskControl = `<input type="text" data-k="mask_id" value="${escHtml(a.mask_id || '')}" placeholder="e.g. Mask 1">`;
    } else {
      let opts = `<option value="">— select mask —</option>`;
      for (const m of privacyMasks) {
        const sel = a.mask_id === m.name ? 'selected' : '';
        opts += `<option value="${escHtml(m.name)}" ${sel}>${escHtml(m.name)}</option>`;
      }
      maskControl = `<select data-k="mask_id">${opts}</select>`;
    }
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Privacy Mask</label>${maskControl}
      </div>
      <div class="form-group" style="flex:0 0 120px;">
        <label>Action</label>
        <select data-k="enabled">
          <option value="true"  ${(a.enabled !== false) ? 'selected':''}>Enable</option>
          <option value="false" ${a.enabled === false   ? 'selected':''}>Disable</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>Channel</label>
        <input type="number" data-k="channel" value="${a.channel || 1}" min="1" max="8">
      </div>
    </div>`;
  }
  if (type === 'wiper') return `
    <div class="form-row">
      <div class="form-group" style="flex:0 0 120px;">
        <label>Operation</label>
        <select data-k="operation">
          <option value="start" ${(a.operation||'start')==='start' ? 'selected':''}>Start</option>
          <option value="stop"  ${a.operation==='stop'             ? 'selected':''}>Stop</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 100px;">
        <label>Device ID</label>
        <input type="number" data-k="id" value="${a.id || 1}" min="1">
        <div class="form-hint">Use <b>getServiceInfo</b> to find IDs.</div>
      </div>
      <div class="form-group" style="flex:0 0 130px;">
        <label>Duration (s, optional)</label>
        <input type="number" data-k="duration" value="${a.duration || ''}" min="1" placeholder="Default">
        <div class="form-hint">Variable-duration wipers only.</div>
      </div>
    </div>`;
  if (type === 'light_control') {
    const op = a.operation || 'on';
    return `
    <div class="form-row">
      <div class="form-group" style="flex:0 0 140px;">
        <label>Operation</label>
        <select data-k="operation" onchange="rerenderAction(this)">
          <option value="on"   ${op==='on'   ? 'selected':''}>On</option>
          <option value="off"  ${op==='off'  ? 'selected':''}>Off</option>
          <option value="auto" ${op==='auto' ? 'selected':''}>Auto</option>
        </select>
      </div>
      <div class="form-group" style="flex:0 0 140px;">
        <label>Light ID</label>
        <input type="text" data-k="light_id" value="${escHtml(a.light_id || 'led0')}" placeholder="led0">
      </div>
      ${op === 'on' ? `
      <div class="form-group" style="flex:0 0 140px;">
        <label>Intensity (0–100, optional)</label>
        <input type="number" data-k="intensity" value="${a.intensity ?? ''}" min="0" max="100" placeholder="Auto">
        <div class="form-hint">Leave blank to use the camera's current intensity setting.</div>
      </div>` : ''}
    </div>`;
  }
  if (type === 'acap_control') {
    let pkgControl;
    if (acapApps === null) {
      pkgControl = `<input type="text" data-k="package" value="${escHtml(a.package || '')}" placeholder="Loading apps…" disabled>`;
    } else if (!acapApps.length) {
      pkgControl = `<input type="text" data-k="package" value="${escHtml(a.package || '')}" placeholder="e.g. com.axis.myapp">`;
    } else {
      let opts = `<option value="">— select app —</option>`;
      for (const app of acapApps) {
        const sel = a.package === app.package ? 'selected' : '';
        opts += `<option value="${escHtml(app.package)}" ${sel}>${escHtml(app.niceName)}</option>`;
      }
      pkgControl = `<select data-k="package">${opts}</select>`;
    }
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Application</label>${pkgControl}
      </div>
      <div class="form-group" style="flex:0 0 140px;">
        <label>Operation</label>
        <select data-k="operation">
          <option value="start"   ${(a.operation||'start')==='start'   ? 'selected':''}>Start</option>
          <option value="stop"    ${a.operation==='stop'               ? 'selected':''}>Stop</option>
          <option value="restart" ${a.operation==='restart'            ? 'selected':''}>Restart</option>
        </select>
      </div>
    </div>`;
  }
  if (type === 'aoa_get_counts') {
    let scenarioControl;
    if (aoaScenarios === null) {
      scenarioControl = `<input type="number" data-k="scenario_id" value="${a.scenario_id || 1}" min="1" placeholder="Loading…">`;
    } else if (!aoaScenarios.length) {
      scenarioControl = `<input type="number" data-k="scenario_id" value="${a.scenario_id || 1}" min="1">`;
    } else {
      const opts = aoaScenarios.map(s => {
        const sel = (a.scenario_id !== undefined ? parseInt(a.scenario_id) === s.id : s.id === 1) ? 'selected' : '';
        return `<option value="${s.id}" ${sel}>${s.id}: ${escHtml(s.name || s.type || 'Scenario ' + s.id)}</option>`;
      }).join('');
      scenarioControl = `<select data-k="scenario_id">${opts}</select>`;
    }
    return `
    <div class="form-row">
      <div class="form-group">
        <label>Scenario</label>
        ${scenarioControl}
        <div class="form-hint">Fetches accumulated crossline counts and injects them as <code>{{aoa_total}}</code>, <code>{{aoa_human}}</code>, <code>{{aoa_car}}</code>, etc.</div>
      </div>
      <div class="form-group" style="flex:0 0 auto;">
        <label style="display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:18px;">
          <span style="font-size:13px;">Reset after fetch</span>
          <label class="toggle">
            <input type="checkbox" data-k="reset_after" ${a.reset_after ? 'checked' : ''}>
            <span class="toggle-slider"></span>
          </label>
        </label>
        <div class="form-hint">Resets the accumulated counts after reading them.</div>
      </div>
    </div>`;
  }
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
        <button class="btn btn-ghost btn-sm" id="atest-${i}" onclick="testActionRow(${i})" title="Send a test — executes this action with sample data now">Test</button>
        <button class="tca-remove" onclick="removeActionRow(${i})">&times;</button>
      </div>
      <div class="tca-fields">${actionFields(a)}</div>
    </div>
  `).join('');
}

function rerenderAction(sel) {
  const row = sel.closest('.tca-row');
  const i = parseInt(row.id.replace('arow-', ''));
  const el = document.getElementById('arow-' + i);
  if (!el) return;
  const data = { type: actionRows[i].type };
  el.querySelectorAll('[data-k]').forEach(inp => {
    data[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
  });
  actionRows[i] = data;
  renderActionList();
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

async function testActionRow(i) {
  /* Collect and normalize current form data for this row */
  const el = document.getElementById('arow-' + i);
  if (!el) return;
  const data = { type: actionRows[i].type };
  el.querySelectorAll('[data-k]').forEach(inp => {
    data[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
  });
  const normalized = normalizeAction(data);
  const btn = document.getElementById('atest-' + i);
  const origText = btn ? btn.textContent : '';
  if (btn) { btn.textContent = '…'; btn.disabled = true; }
  try {
    const resp = await API.testAction(normalized.type, normalized);
    const result = await resp.json().catch(() => ({}));
    if (resp.ok && result.success !== false) {
      toast(`Action ${i + 1} test: OK${result.message ? ' — ' + result.message : ''}`, 'success');
    } else {
      toast(`Action ${i + 1} test failed: ${result.error || result.message || resp.status}`, 'error');
    }
  } catch(e) {
    toast(`Action ${i + 1} test failed: ${e.message}`, 'error');
  } finally {
    if (btn) { btn.textContent = origText; btn.disabled = false; }
  }
}

/* ===== Collect form data ===== */
function collectRows(rows, prefix) {
  return rows.map((_, i) => {
    const el = document.getElementById(`${prefix}-${i}`);
    if (!el) return _;
    const data = { type: _.type };
    el.querySelectorAll('[data-k]').forEach(inp => {
      data[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
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
    const condType = t.cond_type || (t.value_key ? 'numeric' : (t.filter_key ? 'boolean' : (t.string_key ? 'string' : 'none')));
    if (condType === 'boolean') {
      if (t.filter_key) out.filter_key = t.filter_key;
      if (t.filter_value === 'true' || t.filter_value === true)   out.filter_value = true;
      else if (t.filter_value === 'false' || t.filter_value === false) out.filter_value = false;
    } else if (condType === 'numeric') {
      if (t.value_key) out.value_key = t.value_key;
      out.value_op        = t.value_op || 'gt';
      out.value_threshold = parseFloat(t.value_threshold) || 0;
      if (out.value_op === 'between') out.value_threshold2 = parseFloat(t.value_threshold2) || 0;
      const hold = parseInt(t.value_hold_secs) || 0;
      if (hold > 0) out.value_hold_secs = hold;
    } else if (condType === 'string') {
      if (t.string_key)   out.string_key   = t.string_key;
      if (t.string_value) out.string_value = t.string_value;
    }
    if (t.type === 'io_input') {
      out.port = parseInt(t.port) || 1;
      out.edge = t.edge || 'rising';
      const hold = parseInt(t.hold_secs) || 0;
      if (hold > 0) out.hold_secs = hold;
    }
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
    } else if (out.schedule_type === 'astronomical') {
      out.event = t.event || 'sunrise';
      out.latitude = parseFloat(t.latitude) || 0;
      out.longitude = parseFloat(t.longitude) || 0;
      const offMin = parseFloat(t.offset_minutes) || 0;
      if (offMin !== 0) out.offset_minutes = offMin;
    }
  } else if (t.type === 'mqtt_message') {
    out.topic_filter = t.topic_filter || '#';
    if (t.payload_filter) out.payload_filter = t.payload_filter;
  } else if (t.type === 'aoa_scenario') {
    out.scenario_id = parseInt(t.scenario_id) || 1;
    if (t.object_class && t.object_class !== 'any') out.object_class = t.object_class;
  } else if (t.type === 'counter_threshold') {
    out.counter_name = t.counter_name; out.op = t.op || 'gt'; out.value = parseFloat(t.value) || 0;
  } else if (t.type === 'rule_fired') {
    out.rule_id = t.rule_id || '';
  }
  /* manual trigger has no extra fields */
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
    if (c.json_path) { out.json_path = c.json_path; out.json_expected = c.json_expected || ''; }
    if (c.allow_insecure === true) out.allow_insecure = true;
  } else if (c.type === 'aoa_occupancy') {
    out.scenario_id = parseInt(c.scenario_id) || 1;
    out.object_class = c.object_class || 'any';
    out.op = c.op || 'gt';
    out.value = parseFloat(c.value) || 0;
  } else if (c.type === 'day_night') {
    out.state = c.state || 'day';
    if (c.lat !== undefined && c.lat !== '') out.lat = parseFloat(c.lat);
    if (c.lon !== undefined && c.lon !== '') out.lon = parseFloat(c.lon);
  } else if (c.type === 'vapix_event_state') {
    out.event_key = c.event_key || '';
    out.data_key  = c.data_key || '';
    out.expected  = c.expected || '';
  }
  return out;
}

function normalizeAction(a) {
  const out = { type: a.type };
  const pass = ['url','method','headers','body','username','password','operation','duration','text','channel',
                 'preset','port','state','clip_name','message','level','event_id','profile',
                 'seconds','name','value','delta','rule_id','cron','interval_seconds',
                 'schedule_type','time','counter_name','op','token','edge',
                 'topic','payload','qos','position','text_color',
                 'tour_id','parameter','mask_id','light_id','package','id','mode',
                 'webhook_url','title','theme_color','version','database','org','bucket',
                 'measurement','tags','fields','bot_token','chat_id','parse_mode',
                 'smtp_server','from','to','subject','deliver_via','line',
                 'event_key','data_key','expected','interval'];
  pass.forEach(k => { if (a[k] !== undefined && a[k] !== '') out[k] = a[k]; });
  if (out.duration !== undefined) out.duration = parseInt(out.duration) || 0;
  if (out.seconds  !== undefined) out.seconds  = parseInt(out.seconds)  || 1;
  if (out.port     !== undefined) out.port     = parseInt(out.port)     || 1;
  if (out.channel  !== undefined && a.type !== 'slack_webhook') out.channel = parseInt(out.channel) || 1;
  if (out.delta    !== undefined) out.delta    = parseFloat(out.delta)  || 1;
  if (out.value    !== undefined && a.type === 'increment_counter')
    out.delta = parseFloat(out.value) || 0;
  if (a.type === 'fire_vapix_event') {
    const events = Array.isArray(acapEvents) ? acapEvents : [];
    out.event_id = a.event_id || (events[0] ? events[0].id : 'RuleFired');
    const selEvent = events.find(e => e.id === out.event_id);
    if (selEvent && selEvent.state) {
      out.state = a.state === 'true' || a.state === true;
    }
    /* Drop internal UI-only keys */
    delete out._adding;
    delete out._new_id;
    delete out._new_name;
    delete out._new_stateful;
  }
  if (a.type === 'mqtt_publish') {
    out.retain = a.retain === 'true' || a.retain === true;
    out.qos    = parseInt(a.qos) || 0;
    out.attach_snapshot = a.attach_snapshot === true;
  }
  if (a.type === 'vapix_query') {
    /* Reconstruct topic0-3 objects from the hidden _ns / _val fields */
    ['topic0','topic1','topic2','topic3'].forEach(k => {
      const v = a[`${k}_val`];
      if (v) out[k] = { [a[`${k}_ns`] || '']: v };
    });
  }
  if (a.type === 'siren_light') {
    out.signal_action = a.signal_action || 'start';
    out.profile = a.profile || '';
    if (out.signal_action === 'start') out.while_active = a.while_active === true;
  }
  if (a.type === 'recording' && (out.operation || 'start') === 'start')
    out.while_active = a.while_active === true;
  if (a.type === 'overlay_text' && !(parseInt(a.duration) > 0))
    out.while_active = a.while_active === true;
  if (a.type === 'io_output' && !(parseInt(a.duration) > 0))
    out.while_active = a.while_active === true;
  if (a.type === 'privacy_mask')
    out.enabled = a.enabled === 'true' || a.enabled === true;
  if (a.type === 'light_control' && (a.operation || 'on') === 'on' && a.intensity !== '' && a.intensity !== undefined)
    out.intensity = parseInt(a.intensity);
  if (a.type === 'snapshot_upload' || a.type === 'guard_tour' || a.type === 'ir_cut_filter' || a.type === 'privacy_mask')
    if (out.channel !== undefined) out.channel = parseInt(out.channel) || 1;
  if (a.type === 'wiper')
    if (out.id !== undefined) out.id = parseInt(out.id) || 1;
  if (a.type === 'aoa_get_counts') {
    out.scenario_id = parseInt(a.scenario_id) || 1;
    out.reset_after = a.reset_after === true;
  }
  if (a.type === 'influxdb_write') {
    out.version = a.version || 'v2';
  }
  if (a.type === 'email') {
    /* use_tls is controlled by global SMTP config, not per-action */
    out.attach_snapshot = a.attach_snapshot === true;
  }
  if (a.type === 'slack_webhook' || a.type === 'teams_webhook' || a.type === 'telegram') {
    out.attach_snapshot = a.attach_snapshot === true;
  }
  if (a.type === 'telegram') {
    out.disable_preview = a.disable_preview === true;
  }
  if (a.type === 'digest') {
    out.interval = parseInt(a.interval) || 300;
    out.deliver_via = a.deliver_via || 'slack';
  }
  if (a.type === 'http_request') {
    out.attach_snapshot = a.attach_snapshot === true;
    if (a.allow_insecure === true) out.allow_insecure = true;
    /* Fallback action chain */
    const fbType = a.on_failure_type || '';
    if (fbType === 'send_syslog') {
      out.on_failure = [{ type: 'send_syslog', message: a.on_failure_message || 'HTTP request failed' }];
    } else if (fbType === 'mqtt_publish') {
      out.on_failure = [{ type: 'mqtt_publish', topic: a.on_failure_topic || '', payload: a.on_failure_payload || 'http_request_failed', qos: 0, retain: false }];
    } else if (fbType === 'http_request') {
      out.on_failure = [{ type: 'http_request', url: a.on_failure_url || '', method: 'GET' }];
    }
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

  /* Per-action required field validation */
  for (let i = 0; i < actionRows.length; i++) {
    const a = actionRows[i];
    const label = `Action ${i + 1} (${ruleTypeLabel('action', a.type)})`;
    if (a.type === 'http_request'    && !a.url)         { toast(`${label}: URL is required`, 'error'); return; }
    if (a.type === 'mqtt_publish'    && !a.topic)       { toast(`${label}: Topic is required`, 'error'); return; }
    if (a.type === 'email'           && !a.to)          { toast(`${label}: "To" address is required`, 'error'); return; }
    if (a.type === 'slack_webhook'   && !a.webhook_url) { toast(`${label}: Webhook URL is required`, 'error'); return; }
    if (a.type === 'teams_webhook'   && !a.webhook_url) { toast(`${label}: Webhook URL is required`, 'error'); return; }
    if (a.type === 'telegram'        && !a.bot_token)   { toast(`${label}: Bot Token is required`, 'error'); return; }
    if (a.type === 'telegram'        && !a.chat_id)     { toast(`${label}: Chat ID is required`, 'error'); return; }
    if (a.type === 'ftp_upload'      && !a.url)         { toast(`${label}: FTP URL is required`, 'error'); return; }
    if (a.type === 'snapshot_upload' && !a.url)         { toast(`${label}: URL is required`, 'error'); return; }
    if (a.type === 'set_variable'    && !a.name)        { toast(`${label}: Variable name is required`, 'error'); return; }
    if (a.type === 'increment_counter' && !a.name)      { toast(`${label}: Counter name is required`, 'error'); return; }
    if (a.type === 'influxdb_write'  && !a.url)         { toast(`${label}: InfluxDB URL is required`, 'error'); return; }
    if (a.type === 'influxdb_write'  && !a.measurement) { toast(`${label}: Measurement is required`, 'error'); return; }
  }

  const rule = {
    name,
    enabled:         document.getElementById('f-enabled').checked,
    triggers:        triggerRows,
    trigger_logic:   triggerLogic,
    conditions:      conditionRows,
    condition_logic: conditionLogic,
    actions:         actionRows,
    cooldown:         parseInt(document.getElementById('f-cooldown').value) || 0,
    max_executions:   parseInt(document.getElementById('f-maxex').value)    || 0,
    max_exec_period:  document.getElementById('f-maxex-period').value       || '',
    trigger_window:   triggerLogic === 'AND' ? (parseInt(document.getElementById('f-trigger-window').value) || 0) : 0,
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
 * ACAP Event helpers — called from fire_vapix_event action rows
 * =================================================== */

function _acapEventRowIndex(btn) {
  const row = btn.closest('.tca-row');
  if (!row) return -1;
  return parseInt(row.id.replace('arow-', ''));
}

function _acapEventSyncRow(i) {
  const el = document.getElementById('arow-' + i);
  if (!el) return;
  const d = { type: actionRows[i].type };
  el.querySelectorAll('[data-k]').forEach(inp => {
    d[inp.dataset.k] = inp.type === 'checkbox' ? inp.checked : inp.value;
  });
  actionRows[i] = d;
}

function acapEventStartAdd(btn) {
  const i = _acapEventRowIndex(btn);
  if (i < 0) return;
  _acapEventSyncRow(i);
  actionRows[i]._adding = 'true';
  renderActionList();
}

function acapEventCancelAdd(btn) {
  const i = _acapEventRowIndex(btn);
  if (i < 0) return;
  _acapEventSyncRow(i);
  const a = actionRows[i];
  delete a._adding;
  delete a._new_id;
  delete a._new_name;
  delete a._new_stateful;
  renderActionList();
}

async function acapEventSaveNew(btn) {
  const i = _acapEventRowIndex(btn);
  if (i < 0) return;
  _acapEventSyncRow(i);
  const a = actionRows[i];

  const id   = (a._new_id   || '').trim();
  const name = (a._new_name || '').trim();

  if (!id) { toast('Event ID is required', 'error'); return; }
  if (!/^[A-Za-z0-9_]+$/.test(id)) {
    toast('Event ID must contain only letters, digits, and underscores', 'error');
    return;
  }

  try {
    const created = await API.addAcapEvent({
      id,
      name: name || id,
      state: a._new_stateful === 'true',
    });
    await loadAcapEvents();
    delete a._adding;
    delete a._new_id;
    delete a._new_name;
    delete a._new_stateful;
    a.event_id = created.id;
    renderActionList();
    toast(`Event "${created.id}" created`, 'success');
  } catch(e) {
    toast('Failed to create event: ' + e.message, 'error');
  }
}

async function acapEventDelete(id) {
  if (!confirm(`Remove custom event "${id}"? Rules that fire this event will need to be updated.`)) return;
  try {
    await API.deleteAcapEvent(id);
    await loadAcapEvents();
    toast(`Event "${id}" removed`, 'success');
    renderActionList();
  } catch(e) {
    toast('Failed to remove event: ' + e.message, 'error');
  }
}
