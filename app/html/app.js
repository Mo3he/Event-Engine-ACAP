'use strict';

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
let sirenProfiles = null;     /* null=loading, []=none or not supported, [{name,label},...] */
let acapApps = null;          /* null=loading, []=none,   [{package,niceName},...] */
let privacyMasks = null;      /* null=loading, []=none,   [{name},...] */
let deviceParams = null;      /* null=loading, []=none,   ['root.X.Y.Z',...] */
let guardTours = null;        /* null=loading, []=none,   [{id,name},...] */
let aoaScenarios = null;      /* null=loading, []=none or not available, [{id,name,type},...] */
let knownVarNames = [];       /* variable names loaded at startup for hint display */
let knownCounterNames = [];   /* counter names loaded at startup for hint display */
let engineLat = 0;            /* engine settings latitude — used as default for astronomical triggers */
let engineLon = 0;            /* engine settings longitude */

function showFatalUiError(message) {
  const subtitle = document.getElementById('header-subtitle');
  if (subtitle) subtitle.textContent = 'UI error';

  let box = document.getElementById('ui-fatal-error');
  if (!box) {
    box = document.createElement('div');
    box.id = 'ui-fatal-error';
    box.style.cssText =
      'position:fixed;left:12px;right:12px;top:64px;z-index:1200;' +
      'background:#fff1f1;border:1px solid #d34a4a;color:#7a1818;' +
      'padding:10px 12px;border-radius:6px;font:12px/1.4 sans-serif;white-space:pre-wrap';
    document.body.appendChild(box);
  }
  box.textContent = `UI failed to initialize:\n${message}`;
}

window.addEventListener('error', e => {
  const msg = e && (e.message || (e.error && e.error.message)) || 'Unknown script error';
  showFatalUiError(msg);
});

window.addEventListener('unhandledrejection', e => {
  const reason = e && e.reason;
  const msg = (reason && (reason.message || String(reason))) || 'Unhandled promise rejection';
  showFatalUiError(msg);
});

/* Backward compatibility: if an old cached index.html loads only app.js,
 * recreate the API object locally so the UI does not stall on "Loading...". */
if (typeof API === 'undefined') {
  const BASE = '/local/acap_event_engine';
  const _apiFetch = (url, opts) => fetch(url, opts).then(r => {
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r;
  });
  window.API = {
    get:  path => _apiFetch(`${BASE}/${path}`).then(r => r.json()),
    post: (path, body) => _apiFetch(`${BASE}/${path}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }),
    put: (path, body) => _apiFetch(`${BASE}/${path}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    }),
    delete: path => _apiFetch(`${BASE}/${path}`, { method: 'DELETE' }),

    getRules:      ()      => API.get('rules'),
    getRule:       id      => API.get(`rules?id=${encodeURIComponent(id)}`),
    addRule:       rule    => API.post('rules', rule),
    updateRule:    (id, r) => API.post(`rules?id=${encodeURIComponent(id)}`, r),
    deleteRule:    id      => API.delete(`rules?id=${encodeURIComponent(id)}`),
    setEnabled:    (id, v) => API.post(`rules?id=${encodeURIComponent(id)}`, { enabled: v }),
    fireRule:      id      => API.post('fire', { id }),
    testAction:    (type, config) => API.post('actions', { type, config }),

    getEvents:     (limit, rule) => API.get(`events?limit=${limit || 100}${rule ? '&rule=' + encodeURIComponent(rule) : ''}`),
    clearEvents:   ()      => API.delete('events'),
    getStatus:     ()      => API.get('engine'),
    getVariables:  ()      => API.get('variables'),
    setVariable:   (name, value, is_counter) => API.post('variables', { name, value, is_counter: !!is_counter }),
    deleteVariable: name   => API.delete(`variables?name=${encodeURIComponent(name)}`),
  };
}

/* ===================================================
 * Toast
 * =================================================== */
function toast(msg, type = 'info') {
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  if (type === 'error') {
    /* Errors stay until dismissed */
    el.innerHTML = `<span>${escHtml(msg)}</span><button onclick="this.parentElement.remove()" style="background:none;border:none;color:inherit;cursor:pointer;margin-left:10px;font-size:15px;opacity:.8;line-height:1;">&times;</button>`;
  } else {
    el.textContent = msg;
    setTimeout(() => el.remove(), 5000);
  }
  document.getElementById('toast-container').appendChild(el);
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
    if (btn.dataset.tab === 'settings')  { loadStatus(); loadSmtpSettings(); loadProxySettings(); loadMqttSettings(); loadEngineSettings(); }
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

const RULE_TYPE_LABELS = {
  trigger: {
    vapix_event: 'Camera Event', schedule: 'Schedule', mqtt_message: 'MQTT',
    http_webhook: 'Webhook', io_input: 'I/O Input', counter_threshold: 'Counter',
    rule_fired: 'Rule Fired', aoa_scenario: 'AOA Scenario', manual: 'Manual'
  },
  condition: {
    time_window: 'Time Window', io_state: 'I/O State', counter: 'Counter',
    variable_compare: 'Variable', http_check: 'HTTP Check', aoa_occupancy: 'AOA Occupancy',
    day_night: 'Day/Night', vapix_event_state: 'Event State'
  },
  action: {
    http_request: 'HTTP', mqtt_publish: 'MQTT', recording: 'Recording',
    overlay_text: 'Overlay', ptz_preset: 'PTZ', io_output: 'I/O Output',
    audio_clip: 'Audio', siren_light: 'Siren/Light', vapix_query: 'Event Query',
    set_variable: 'Set Variable', increment_counter: 'Counter', run_rule: 'Run Rule',
    delay: 'Delay', fire_vapix_event: 'VAPIX Event', send_syslog: 'Syslog',
    aoa_get_counts: 'AOA Counts', slack_webhook: 'Slack', teams_webhook: 'Teams',
    influxdb_write: 'InfluxDB', telegram: 'Telegram', email: 'Email',
    ftp_upload: 'FTP Upload', digest: 'Digest', snapshot_upload: 'Snapshot',
    guard_tour: 'Guard Tour', ir_cut_filter: 'IR Cut', light_control: 'Light',
    privacy_mask: 'Privacy Mask', wiper: 'Wiper', set_device_param: 'Device Param',
    acap_control: 'ACAP Control'
  }
};
function ruleTypeLabel(group, type) {
  return (RULE_TYPE_LABELS[group] && RULE_TYPE_LABELS[group][type]) || type;
}

function escHtml(s) {
  return String(s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;')
    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

/* ===================================================
 * VAPIX Event Catalog — fetched from the camera at startup
 * =================================================== */
const RULE_TEMPLATES = [
  {
    name: 'Camera Event → HTTP Webhook',
    icon: '🌐',
    desc: 'POST to a URL whenever a VAPIX camera event fires',
    rule: {
      name: 'Camera Event → HTTP Webhook',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'vapix_event' }],
      conditions: [],
      actions: [{ type: 'http_request', method: 'POST',
                  url: 'https://example.com/webhook',
                  headers: 'Content-Type: application/json',
                  body: '{"camera":"{{camera.serial}}","time":"{{timestamp}}","data":{{trigger_json}}}' }],
    }
  },
  {
    name: 'Daily Schedule → Syslog',
    icon: '🕗',
    desc: 'Log a message every weekday at 08:00',
    rule: {
      name: 'Daily Schedule → Syslog',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'schedule', schedule_type: 'daily_time', time: '08:00', days: [1,2,3,4,5] }],
      conditions: [],
      actions: [{ type: 'send_syslog', message: 'Daily check at {{timestamp}} on {{camera.model}}', level: 'info' }],
    }
  },
  {
    name: 'Camera Event → Email Alert',
    icon: '✉️',
    desc: 'Send an email when a camera event fires (configure SMTP in Settings first)',
    rule: {
      name: 'Camera Event → Email Alert',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'vapix_event' }],
      conditions: [],
      actions: [{ type: 'email', to: 'alerts@example.com',
                  subject: 'Alert from {{camera.model}}',
                  body: 'An event occurred at {{timestamp}}.\nCamera: {{camera.serial}} ({{camera.ip}})' }],
    }
  },
  {
    name: 'Camera Event → Slack',
    icon: '💬',
    desc: 'Post a Slack message when a camera event fires',
    rule: {
      name: 'Camera Event → Slack',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'vapix_event' }],
      conditions: [],
      actions: [{ type: 'slack_webhook', webhook_url: '',
                  message: 'Alert from {{camera.model}} at {{timestamp}}' }],
    }
  },
  {
    name: 'MQTT Message → Set Variable',
    icon: '📡',
    desc: 'Store the payload of an incoming MQTT message as a variable',
    rule: {
      name: 'MQTT Message → Set Variable',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'mqtt_message', topic_filter: 'home/+/status' }],
      conditions: [],
      actions: [{ type: 'set_variable', name: 'last_mqtt_payload', value: '{{trigger.payload}}' }],
    }
  },
  {
    name: 'I/O Input → I/O Output',
    icon: '⚡',
    desc: 'Activate an output port for 5 s when an input port goes high',
    rule: {
      name: 'I/O Input → I/O Output',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'io_input', port: 1, edge: 'rising', hold_secs: 0 }],
      conditions: [],
      actions: [{ type: 'io_output', port: 1, state: 'active', duration: 5 }],
    }
  },
  {
    name: 'Hourly Snapshot → FTP Upload',
    icon: '📷',
    desc: 'Upload a camera snapshot to an FTP server every hour',
    rule: {
      name: 'Hourly Snapshot → FTP Upload',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'schedule', schedule_type: 'cron', cron: '0 * * * *' }],
      conditions: [],
      actions: [{ type: 'ftp_upload',
                  url: 'ftp://server/cameras/{{camera.serial}}/{{date}}_{{time}}.jpg',
                  username: '', password: '' }],
    }
  },
  {
    name: 'Manual → PTZ + Overlay',
    icon: '🎯',
    desc: 'Move to a PTZ preset and show a text overlay — triggered by the Fire button or API',
    rule: {
      name: 'Manual → PTZ + Overlay',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'manual' }],
      conditions: [],
      actions: [
        { type: 'ptz_preset', preset: 'HomePosition', channel: 1 },
        { type: 'overlay_text', text: 'Home at {{time}}', channel: 1, duration: 5, position: 'topLeft', text_color: 'white' }
      ],
    }
  },
  {
    name: 'Camera Event → Telegram',
    icon: '✈️',
    desc: 'Send a Telegram message when a camera event fires',
    rule: {
      name: 'Camera Event → Telegram',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'vapix_event' }],
      conditions: [],
      actions: [{ type: 'telegram', bot_token: '', chat_id: '',
                  message: '📷 Alert from *{{camera.model}}*\nTime: {{timestamp}}\nCamera: {{camera.serial}}',
                  parse_mode: 'Markdown' }],
    }
  },
  {
    name: 'Camera Event → Microsoft Teams',
    icon: '🏢',
    desc: 'Post an adaptive card to a Teams channel via Power Automate webhook',
    rule: {
      name: 'Camera Event → Microsoft Teams',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'vapix_event' }],
      conditions: [],
      actions: [{ type: 'teams_webhook', webhook_url: '',
                  title: 'Alert from {{camera.model}}',
                  message: 'An event was detected at {{timestamp}} on camera {{camera.serial}} ({{camera.ip}}).' }],
    }
  },
  {
    name: 'Motion → Recording (Business Hours)',
    icon: '📹',
    desc: 'Start a 30 s recording when motion is detected, but only during weekday business hours',
    rule: {
      name: 'Motion → Recording (Business Hours)',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{
        type: 'vapix_event',
        topic0: { tnsaxis: 'VideoAnalytics' },
        topic1: { tnsaxis: 'MotionDetection' },
        filter_key: 'active', filter_value: true
      }],
      conditions: [{ type: 'time_window', start: '08:00', end: '18:00', days: [1,2,3,4,5] }],
      actions: [{ type: 'recording', operation: 'start', duration: 30 }],
      cooldown: 30,
    }
  },
  {
    name: 'Arm System via MQTT',
    icon: '🔒',
    desc: 'Set system.armed = true when an MQTT "arm" command arrives — pair with a condition on other rules',
    rule: {
      name: 'Arm System via MQTT',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'mqtt_message', topic_filter: 'cameras/{{camera.serial}}/arm', payload_filter: 'arm' }],
      conditions: [],
      actions: [
        { type: 'set_variable', name: 'system.armed', value: 'true' },
        { type: 'send_syslog', message: 'System ARMED via MQTT at {{timestamp}}', level: 'info' }
      ],
    }
  },
  {
    name: 'Disarm System via MQTT',
    icon: '🔓',
    desc: 'Set system.armed = false when an MQTT "disarm" command arrives',
    rule: {
      name: 'Disarm System via MQTT',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'mqtt_message', topic_filter: 'cameras/{{camera.serial}}/arm', payload_filter: 'disarm' }],
      conditions: [],
      actions: [
        { type: 'set_variable', name: 'system.armed', value: 'false' },
        { type: 'send_syslog', message: 'System DISARMED via MQTT at {{timestamp}}', level: 'info' }
      ],
    }
  },
  {
    name: 'Motion Alert When Armed',
    icon: '🚨',
    desc: 'Fire an MQTT alert on motion, but only when system.armed = true',
    rule: {
      name: 'Motion Alert When Armed',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{
        type: 'vapix_event',
        topic0: { tnsaxis: 'VideoAnalytics' },
        topic1: { tnsaxis: 'MotionDetection' },
        filter_key: 'active', filter_value: true
      }],
      conditions: [{ type: 'variable_compare', name: 'system.armed', op: 'eq', value: 'true' }],
      actions: [{
        type: 'mqtt_publish',
        topic: 'cameras/{{camera.serial}}/alert',
        payload: '{"event":"motion","camera":"{{camera.serial}}","time":"{{timestamp}}"}',
        qos: 1, retain: false
      }],
      cooldown: 30,
    }
  },
  {
    name: 'AOA Object Detection → MQTT',
    icon: '🚗',
    desc: 'Publish to MQTT whenever Object Analytics detects an object in a scenario',
    rule: {
      name: 'AOA Object Detection → MQTT',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'aoa_scenario', scenario_id: 1, object_class: 'any' }],
      conditions: [],
      actions: [{
        type: 'mqtt_publish',
        topic: 'cameras/{{camera.serial}}/aoa',
        payload: '{"scenario":"{{trigger.scenario_id}}","class":"{{trigger.object_class}}","time":"{{timestamp}}"}',
        qos: 0, retain: false
      }],
      cooldown: 5,
    }
  },
  {
    name: 'Schedule → InfluxDB Sensor Write',
    icon: '📈',
    desc: 'Poll a VAPIX sensor value every 5 minutes and write it to InfluxDB',
    rule: {
      name: 'Schedule → InfluxDB Sensor Write',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'schedule', schedule_type: 'interval', interval_seconds: 300 }],
      conditions: [],
      actions: [
        { type: 'vapix_query' },
        { type: 'influxdb_write',
          url: 'http://influxdb:8086', version: 'v2',
          org: 'my-org', bucket: 'cameras', token: '',
          measurement: 'camera_sensors',
          tags: 'camera={{camera.serial}},model={{camera.model}}',
          fields: 'value={{trigger.Value}}' }
      ],
    }
  },
  {
    name: 'Camera Event → Notification Digest',
    icon: '📨',
    desc: 'Batch events and send one summary message every 5 minutes instead of one alert per event',
    rule: {
      name: 'Camera Event → Notification Digest',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'vapix_event' }],
      conditions: [],
      actions: [{
        type: 'digest', deliver_via: 'slack', webhook_url: '',
        interval: 300,
        line: '{{timestamp}} — {{trigger_json}}'
      }],
    }
  },
  {
    name: 'Sunrise → IR Cut Filter Day Mode',
    icon: '🌅',
    desc: 'Switch the IR cut filter to Day mode at sunrise each morning',
    rule: {
      name: 'Sunrise → IR Cut Filter Day Mode',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'schedule', schedule_type: 'astronomical', event: 'sunrise', offset_minutes: 0, latitude: 0, longitude: 0 }],
      conditions: [],
      actions: [{ type: 'ir_cut_filter', mode: 'day', channel: 1 }],
    }
  },
  {
    name: 'Sunset → IR Cut Filter Night Mode',
    icon: '🌙',
    desc: 'Switch the IR cut filter to Night mode at sunset each evening',
    rule: {
      name: 'Sunset → IR Cut Filter Night Mode',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'schedule', schedule_type: 'astronomical', event: 'sunset', offset_minutes: 0, latitude: 0, longitude: 0 }],
      conditions: [],
      actions: [{ type: 'ir_cut_filter', mode: 'night', channel: 1 }],
    }
  },
  {
    name: 'Counter Threshold → Slack Alert',
    icon: '🔢',
    desc: 'Send a Slack message when a counter exceeds a threshold',
    rule: {
      name: 'Counter Threshold → Slack Alert',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'counter_threshold', counter_name: 'my_counter', op: 'gte', value: 10 }],
      conditions: [],
      actions: [{ type: 'slack_webhook', webhook_url: '',
                  message: 'Counter *{{trigger.counter_name}}* reached {{trigger.counter_value}} on {{camera.model}} at {{timestamp}}' }],
      cooldown: 60,
    }
  },
  {
    name: 'I/O Input → Siren + MQTT',
    icon: '🔔',
    desc: 'Trigger the siren/light and publish an MQTT alert when an input port activates',
    rule: {
      name: 'I/O Input → Siren + MQTT',
      enabled: true, trigger_logic: 'OR', condition_logic: 'AND',
      triggers: [{ type: 'io_input', port: 1, edge: 'rising', hold_secs: 0 }],
      conditions: [],
      actions: [
        { type: 'siren_light', signal_action: 'start', profile: '', while_active: true },
        { type: 'mqtt_publish', topic: 'cameras/{{camera.serial}}/io/{{trigger.port}}',
          payload: '{"port":"{{trigger.port}}","state":"{{trigger.state}}","time":"{{timestamp}}"}',
          qos: 0, retain: false }
      ],
    }
  },
];

function openTemplateModal() {
  const existing = document.getElementById('_template_panel');
  if (existing) { existing.remove(); return; }

  const panel = document.createElement('div');
  panel.id = '_template_panel';
  panel.style.cssText =
    'position:fixed;z-index:9999;background:var(--surface);border:1px solid var(--border);' +
    'border-radius:8px;box-shadow:var(--shadow);padding:0;min-width:320px;max-width:380px;overflow:hidden;';

  const header = `<div style="padding:12px 16px;border-bottom:1px solid var(--border);font-size:13px;font-weight:600;">
    Create from Template
    <button onclick="document.getElementById('_template_panel').remove()" style="float:right;background:none;border:none;color:var(--text-muted);cursor:pointer;font-size:16px;">&times;</button>
  </div>`;
  const templateIconByTrigger = {
    vapix_event: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M480-280q83 0 141.5-58.5T680-480q0-83-58.5-141.5T480-680q-83 0-141.5 58.5T280-480q0 83 58.5 141.5T480-280Zm0 120q-134 0-227-93t-93-227q0-134 93-227t227-93q134 0 227 93t93 227q0 134-93 227t-227 93Zm0-200q50 0 85-35t35-85q0-50-35-85t-85-35q-50 0-85 35t-35 85q0 50 35 85t85 35Zm0-120Z"/></svg>',
    schedule: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M200-80q-33 0-56.5-23.5T120-160v-560q0-33 23.5-56.5T200-800h40v-80h80v80h320v-80h80v80h40q33 0 56.5 23.5T840-720v560q0 33-23.5 56.5T760-80H200Zm0-80h560v-400H200v400Zm280-120q17 0 28.5-11.5T520-320q0-17-11.5-28.5T480-360q-17 0-28.5 11.5T440-320q0 17 11.5 28.5T480-280Zm-40-160h80v-200h-80v200ZM200-640h560v-80H200v80Zm0 0v-80 80Z"/></svg>',
    mqtt_message: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M480-120q-17 0-28.5-11.5T440-160q0-17 11.5-28.5T480-200q17 0 28.5 11.5T520-160q0 17-11.5 28.5T480-120Zm-40-120v-240h80v240h-80Zm-85-325-57-57q37-37 83-57.5T480-700q53 0 99.5 20.5T663-622l-57 57q-26-26-58.5-40.5T480-620q-35 0-67.5 14.5T355-565Zm-113-113-57-57q60-60 134-92.5T480-860q87 0 161 32.5T775-735l-57 57q-48-48-106.5-75T480-780q-73 0-131.5 27T242-678Z"/></svg>',
    io_input: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M280-80v-360H120l320-440v360h160L280-80Z"/></svg>',
    manual: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M320-200v-560l440 280-440 280Z"/></svg>',
    aoa_scenario: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M480-120q-75 0-140.5-28.5t-114-77q-48.5-48.5-77-114T120-480q0-75 28.5-140.5t77-114q48.5-48.5 114-77T480-840q75 0 140.5 28.5t114 77q48.5 48.5 77 114T840-480q0 75-28.5 140.5t-77 114q-48.5 48.5-114 77T480-120Zm0-80q117 0 198.5-81.5T760-480q0-117-81.5-198.5T480-760q-117 0-198.5 81.5T200-480q0 117 81.5 198.5T480-200Zm0-120q67 0 113.5-46.5T640-480q0-67-46.5-113.5T480-640q-67 0-113.5 46.5T320-480q0 67 46.5 113.5T480-320Z"/></svg>',
    counter_threshold: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M280-200h160v-80H280v80Zm0-160h400v-80H280v80Zm0-160h400v-80H280v80ZM200-80q-33 0-56.5-23.5T120-160v-640q0-33 23.5-56.5T200-880h560q33 0 56.5 23.5T840-800v640q0 33-23.5 56.5T760-80H200Z"/></svg>',
    default: '<svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M320-240h320v-80H320v80Zm0-160h320v-80H320v80Zm0-160h200v-80H320v80ZM240-80q-33 0-56.5-23.5T160-160v-640q0-33 23.5-56.5T240-880h320l240 240v480q0 33-23.5 56.5T720-80H240Zm280-520v-200H240v640h480v-440H520Z"/></svg>'
  };

  const items = RULE_TEMPLATES.map((t, i) => {
    const triggerType = t.rule && t.rule.triggers && t.rule.triggers[0] && t.rule.triggers[0].type;
    const iconSvg = templateIconByTrigger[triggerType] || templateIconByTrigger.default;
    return (
    `<div onclick="applyTemplate(${i})" style="padding:10px 16px;cursor:pointer;border-bottom:1px solid var(--border);display:flex;gap:12px;align-items:flex-start;"
          onmouseover="this.style.background='var(--surface2)'" onmouseout="this.style.background=''">
      <span style="display:inline-flex;align-items:center;justify-content:center;flex-shrink:0;margin-top:1px;color:var(--accent);">${iconSvg}</span>
      <div>
        <div style="font-size:13px;font-weight:500;">${escHtml(t.name)}</div>
        <div style="font-size:11px;color:var(--text-muted);margin-top:2px;">${escHtml(t.desc)}</div>
      </div>
    </div>`
    );
  }).join('');
  panel.innerHTML = header + `<div style="max-height:380px;overflow-y:auto;">${items}</div>`;
  document.body.appendChild(panel);

  /* Position below whichever template button triggered it */
  const btn = document.getElementById('btn-template') || document.getElementById('btn-template-empty');
  if (btn) {
    const rect = btn.getBoundingClientRect();
    const pw = 380;
    const left = Math.min(rect.left, window.innerWidth - pw - 10);
    panel.style.left = Math.max(10, left) + 'px';
    panel.style.top  = (rect.bottom + 4) + 'px';
  } else {
    panel.style.left = '50%';
    panel.style.top  = '80px';
    panel.style.transform = 'translateX(-50%)';
  }

  setTimeout(() => {
    document.addEventListener('click', function _close(e) {
      const p = document.getElementById('_template_panel');
      if (p && !p.contains(e.target) &&
          e.target.id !== 'btn-template' && e.target.id !== 'btn-template-empty') {
        p.remove();
      }
      document.removeEventListener('click', _close);
    });
  }, 0);
}

function applyTemplate(i) {
  const p = document.getElementById('_template_panel');
  if (p) p.remove();
  const tmpl = RULE_TEMPLATES[i];
  if (!tmpl) return;
  /* Deep-copy so each use is independent */
  openRuleEditor(JSON.parse(JSON.stringify(tmpl.rule)));
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
      <p style="margin-bottom:20px;">Create your first rule from scratch or start with a ready-made template.</p>
      <div style="display:flex;gap:12px;justify-content:center;flex-wrap:wrap;">
        <button class="btn btn-template" onclick="openTemplateModal()" id="btn-template-empty"><svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M320-240q-33 0-56.5-23.5T240-320v-480q0-33 23.5-56.5T320-880h400q33 0 56.5 23.5T800-800v80h80q33 0 56.5 23.5T960-640v400q0 33-23.5 56.5T880-160H400q-33 0-56.5-23.5T320-240Zm0-80h400v-480H320v480Zm480 0v-400H400v80h320q33 0 56.5 23.5T800-560v320h-80Z"/></svg> From Template</button>
        <button class="btn btn-primary" onclick="openRuleEditor(null)">+ New Rule</button>
      </div>
    </div>`;
    return;
  }

  const searchEl = document.getElementById('rule-search');
  const search = searchEl ? searchEl.value.toLowerCase().trim() : '';
  const visibleRules = search ? allRules.filter(r => r.name.toLowerCase().includes(search)) : allRules;

  if (!visibleRules.length && search) {
    list.innerHTML = `<div style="text-align:center;padding:40px;color:var(--text-muted);">No rules match "<em>${escHtml(search)}</em>"</div>`;
    return;
  }

  list.innerHTML = visibleRules.map(r => {
    const isManualOnly = (r.trigger_types||[]).length > 0 && (r.trigger_types||[]).every(t => t === 'manual');
    const fireBtn = isManualOnly
      ? `<button class="btn btn-primary btn-sm" onclick="testRule('${r.id}')" title="Fire rule now"><svg xmlns="http://www.w3.org/2000/svg" height="18px" viewBox="0 -960 960 960" width="18px" fill="currentColor"><path d="M320-200v-560l440 280-440 280Z"/></svg> Fire</button>`
      : `<button class="btn btn-ghost btn-sm btn-icon" onclick="testRule('${r.id}')" title="Test / Fire now"><svg xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M320-200v-560l440 280-440 280Z"/></svg></button>`;
    return `
    <div class="rule-card ${r.enabled ? '' : 'disabled'}" id="rule-card-${r.id}">
      <label class="toggle" title="${r.enabled ? 'Disable' : 'Enable'} rule">
        <input type="checkbox" ${r.enabled ? 'checked' : ''} onchange="toggleRule('${r.id}', this.checked)">
        <span class="toggle-slider"></span>
      </label>
      <div class="rule-meta">
        <div class="rule-name">${escHtml(r.name)}</div>
        <div class="rule-badges">
          ${(r.trigger_types||[]).map(t => `<span class="badge badge-trigger">${escHtml(ruleTypeLabel('trigger',t))}</span>`).join('')}
          ${(r.condition_types||[]).map(t => `<span class="badge badge-cond">${escHtml(ruleTypeLabel('condition',t))}</span>`).join('')}
          ${(r.action_types||[]).map(t => `<span class="badge badge-action">${escHtml(ruleTypeLabel('action',t))}</span>`).join('')}
          ${r.cooldown ? `<span class="badge">cooldown ${r.cooldown}s</span>` : ''}
        </div>
      </div>
      <div class="rule-fired-time">${r.last_fired ? '<svg xmlns="http://www.w3.org/2000/svg" height="16px" viewBox="0 -960 960 960" width="16px" fill="currentColor" style="vertical-align:middle;margin-right:4px"><path d="M280-80v-360H120l320-440v360h160L280-80Z"/></svg>' + fmtTime(r.last_fired) : 'Never fired'}</div>
      <div class="rule-actions">
        ${fireBtn}
        <button class="btn btn-ghost btn-sm btn-icon" onclick="editRule('${r.id}')" title="Edit"><svg xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M200-200h57l391-391-57-57-391 391v57Zm-80 80v-170l528-527q12-12 27-17.5t30-5.5q16 0 31 5.5t27 17.5l57 57q12 12 18 27.5t6 30.5q0 15-6 30t-18 27L290-120H120Zm640-583-57-57 57 57ZM619-619l-28-29 57 57-29-28Z"/></svg></button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="exportRule('${r.id}')" title="Export"><svg xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M240-80q-33 0-56.5-23.5T160-160v-400q0-33 23.5-56.5T240-640h120v80H240v400h480v-400H600v-80h120q33 0 56.5 23.5T800-560v400q0 33-23.5 56.5T720-80H240Zm200-240v-447l-64 64-56-57 160-160 160 160-56 57-64-64v447h-80Z"/></svg></button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="duplicateRule('${r.id}')" title="Duplicate"><svg xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M360-240q-33 0-56.5-23.5T280-320v-480q0-33 23.5-56.5T360-880h360q33 0 56.5 23.5T800-800v480q0 33-23.5 56.5T720-240H360Zm0-80h360v-480H360v480ZM200-80q-33 0-56.5-23.5T120-160v-560h80v560h440v80H200Zm160-240v-480 480Z"/></svg></button>
        <button class="btn btn-ghost btn-sm btn-icon" onclick="deleteRule('${r.id}')" title="Delete"><svg xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M280-120q-33 0-56.5-23.5T200-200v-520h-40v-80h120v-40h440v40h120v80h-40v520q0 33-23.5 56.5T680-120H280Zm400-600H280v520h400v-520ZM360-280h80v-360h-80v360Zm160 0h80v-360h-80v360ZM280-720v520-520Z"/></svg></button>
      </div>
    </div>`;
  }).join('');
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
  try {
    const rule = await API.getRule(id);
    openRuleEditor(rule);
  } catch(e) {
    toast('Failed to load rule: ' + e.message, 'error');
  }
}

async function duplicateRule(id) {
  try {
    const rule = await API.getRule(id);
    const { id: _id, execution_count: _ec, last_fired: _lf, ...copy } = rule;
    copy.name = copy.name + ' (copy)';
    copy.enabled = true;
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

async function exportRule(id) {
  try {
    const r = allRules.find(r => r.id === id);
    const ruleName = r ? r.name : id;
    const response = await fetch(`/local/acap_event_engine/rules?id=${encodeURIComponent(id)}&action=export`);
    if (!response.ok) {
      toast('Failed to export rule', 'error');
      return;
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = ruleName.replace(/[^a-z0-9_-]/gi, '_') + '.json';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    toast('Rule exported', 'success');
  } catch(e) {
    toast('Failed to export rule: ' + e.message, 'error');
  }
}

/* ===================================================
 * Rule Editor Modal
 * =================================================== */

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

async function clearEventLog() {
  if (!confirm('Clear all event log entries?')) return;
  try {
    await API.clearEvents();
    renderEventLog([]);
    toast('Event log cleared', 'success');
  } catch(e) {
    toast('Failed to clear log', 'error');
  }
}

function renderEventLog(events) {
  const tbody = document.getElementById('event-log-body');
  if (!events.length) {
    tbody.innerHTML = '<tr><td colspan="4" style="text-align:center;padding:30px;color:var(--text-muted);">No events yet</td></tr>';
    return;
  }
  /* Sort by timestamp descending (newest first) to ensure chronological order */
  events.sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
  tbody.innerHTML = events.map((e, idx) => {
    const d = new Date(e.timestamp * 1000);
    const time = d.toLocaleTimeString([], { hour12: false }) + ' ' +
                 d.toLocaleDateString([], { month: 'short', day: 'numeric' });
    const badge = e.fired
      ? '<span class="badge event-badge-fired">Fired</span>'
      : `<span class="badge event-badge-blocked">${escHtml(e.block_reason || 'Blocked')}</span>`;
    const detailObj = e.trigger_data || null;
    const detailStr = detailObj ? JSON.stringify(detailObj, null, 2) : '';
    const hasError = e.actions_failed > 0 && e.action_error;
    const expandId = `log-detail-${idx}`;
    const hasDetail = !!(detailStr || hasError);
    // Short summary shown inline
    let shortDetail = '';
    if (hasError) shortDetail = e.action_error;
    else if (detailStr) shortDetail = JSON.stringify(detailObj).slice(0, 80) + (JSON.stringify(detailObj).length > 80 ? '…' : '');
    // Expanded content
    let expandedHtml = '';
    if (hasError) expandedHtml += `<div style="color:var(--error,#f87171);margin-bottom:${detailStr ? '8px' : '0'};font-size:12px;"><svg xmlns="http://www.w3.org/2000/svg" height="16px" viewBox="0 -960 960 960" width="16px" fill="currentColor" style="vertical-align:middle;margin-right:4px"><path d="M40-120l440-760 440 760H40Zm138-80h604L480-720 178-200Zm302-40q17 0 28.5-11.5T520-280q0-17-11.5-28.5T480-320q-17 0-28.5 11.5T440-280q0 17 11.5 28.5T480-240Zm-40-120h80v-200h-80v200Z"/></svg> ${escHtml(e.action_error)}</div>`;
    if (detailStr) expandedHtml += `<pre style="font-size:11px;color:var(--text-muted);margin:0;white-space:pre-wrap;word-break:break-all;">${escHtml(detailStr)}</pre>`;
    return `<tr style="${hasDetail ? 'cursor:pointer;' : ''}" onclick="${hasDetail ? `toggleLogDetail('${expandId}')` : ''}">
      <td style="white-space:nowrap;color:var(--text-muted)">${time}</td>
      <td>${escHtml(e.rule_name || e.rule_id)}</td>
      <td>${badge}</td>
      <td><code style="font-size:11px;color:${hasError ? 'var(--error,#f87171)' : 'var(--text-muted)'}">​${escHtml(shortDetail)}</code>${hasDetail ? ' <svg xmlns="http://www.w3.org/2000/svg" height="14px" viewBox="0 -960 960 960" width="14px" fill="currentColor" style="vertical-align:-2px;opacity:.4"><path d="M480-345 240-585l56-56 184 184 184-184 56 56-240 240Z"/></svg>' : ''}</td>
    </tr>
    ${hasDetail ? `<tr id="${expandId}" style="display:none;">
      <td colspan="4" style="padding:8px 16px 12px;background:var(--surface);border-top:none;">
        ${expandedHtml}
      </td>
    </tr>` : ''}`;
  }).join('');
}

function toggleLogDetail(id) {
  const el = document.getElementById(id);
  if (el) el.style.display = el.style.display === 'none' ? '' : 'none';
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
function downloadJSON(filename, data) {
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url; a.download = filename; a.click();
  URL.revokeObjectURL(url);
}

async function exportRules() {
  try {
    const summaries = await API.getRules();
    const full = await Promise.all(summaries.map(r => API.getRule(r.id)));
    downloadJSON('event_engine_rules.json', full);
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
      try {
        await API.addRule(r);
        ok++;
      } catch(_) { fail++; }
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
    if (tab === 'rules')    loadRules();
    else if (tab === 'log') loadEvents();
    /* settings tab: only refresh the status widgets, not the forms */
    else if (tab === 'settings') {
      loadStatus();
      API.getStatus().then(s => updateMqttStatusBadge((s && s.mqtt) || {})).catch(() => {});
    }
  }, 10000);
}

/* ===================================================
 * Init
 * =================================================== */
/* ===================================================
 * Theme
 * =================================================== */
function updateThemeButton(theme) {
  const btn = document.getElementById('theme-toggle');
  if (btn) btn.textContent = theme === 'dark' ? '☀' : '☾';
}

function toggleTheme() {
  const current = document.documentElement.getAttribute('data-theme') || 'dark';
  const next = current === 'dark' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', next);
  localStorage.setItem('theme', next);
  updateThemeButton(next);
  const fav = document.getElementById('favicon');
  if (fav) fav.href = next === 'light' ? 'event_engine_icon_dark.svg' : 'event_engine_icon_light.svg';
  const appIcon = document.getElementById('app-icon');
  if (appIcon) appIcon.src = next === 'light' ? 'event_engine_icon_dark.svg' : 'event_engine_icon_light.svg';
}

/* Close modal on ESC key */
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') {
    const overlay = document.getElementById('modal-overlay');
    if (overlay && !overlay.classList.contains('hidden')) closeModal();
  }
});

window.addEventListener('DOMContentLoaded', () => {
  try {
    const subtitle = document.getElementById('header-subtitle');
    if (subtitle) subtitle.textContent = 'Initializing...';

    const initTheme = document.documentElement.getAttribute('data-theme') || 'dark';
    updateThemeButton(initTheme);
    const appIcon = document.getElementById('app-icon');
    if (appIcon) appIcon.src = initTheme === 'light' ? 'event_engine_icon_dark.svg' : 'event_engine_icon_light.svg';

    loadRules();
    loadStatus();
    startPoll();
    loadVapixEventCatalog();
    Promise.all([
      loadPtzPresets(),
      loadAudioClips(),
      loadSirenProfiles(),
      loadAcapApps(),
      loadPrivacyMasks(),
      loadDeviceParams(),
      loadGuardTours(),
      loadAoaScenarios()
    ]).then(() => renderDeviceCapabilities()).catch(() => {});

    /* Load engine lat/lon early so astronomical trigger defaults are correct */
    API.get('settings').then(s => {
      const eng = (s && s.engine) || {};
      if (eng.latitude  !== undefined) engineLat = eng.latitude;
      if (eng.longitude !== undefined) engineLon = eng.longitude;
    }).catch(() => {});

    API.getVariables().then(v => {
      knownVarNames     = Object.keys(v || {}).filter(k => !(v[k] && v[k].is_counter));
      knownCounterNames = Object.keys(v || {}).filter(k =>   v[k] && v[k].is_counter);
    }).catch(() => {});

    window.__appBooted = true;
  } catch (e) {
    showFatalUiError(e && e.message ? e.message : String(e));
  }
});
