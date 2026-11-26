/*
 * Offline preview fixtures only: all device names, event details, addresses,
 * settings, and rules below are fabricated. To refresh after a UI change,
 * recopy app/html UI assets into docs/preview and update these fixtures only.
 */
(() => {
  'use strict';

  const now = Math.floor(Date.now() / 1000);
  const state = {
    rules: [
      {
        id: 'preview-rule-motion',
        name: 'Sample motion → indicator',
        enabled: true,
        trigger_logic: 'OR',
        condition_logic: 'AND',
        triggers: [{ type: 'vapix_event', topic: 'sample/motion' }],
        conditions: [],
        actions: [{ type: 'io_output', port: 1, state: true }],
        trigger_types: ['vapix_event'],
        condition_types: [],
        action_types: ['io_output'],
        last_fired: now - 240
      },
      {
        id: 'preview-rule-schedule',
        name: 'Sample weekday status note',
        enabled: true,
        trigger_logic: 'OR',
        condition_logic: 'AND',
        triggers: [{ type: 'schedule', schedule_type: 'daily_time', time: '09:00', days: [1, 2, 3, 4, 5] }],
        conditions: [],
        actions: [{ type: 'send_syslog', message: 'Preview-only sample event' }],
        trigger_types: ['schedule'],
        condition_types: [],
        action_types: ['send_syslog'],
        last_fired: now - 3600
      },
      {
        id: 'preview-rule-manual',
        name: 'Sample manual test',
        enabled: false,
        trigger_logic: 'OR',
        condition_logic: 'AND',
        triggers: [{ type: 'manual' }],
        conditions: [],
        actions: [{ type: 'set_variable', name: 'preview_mode', value: 'sample' }],
        trigger_types: ['manual'],
        condition_types: [],
        action_types: ['set_variable']
      }
    ],
    events: [
      {
        kind: 'rule', timestamp: now - 240, last_timestamp: now - 240, rule_id: 'preview-rule-motion',
        rule_name: 'Sample motion → indicator', fired: true,
        trigger_data: { source: 'fictional sensor', zone: 'sample-zone-1' }
      },
      {
        kind: 'system', timestamp: now - 720, last_timestamp: now - 720, source: 'engine',
        level: 'info', message: 'Preview engine is using synthetic data.', repeats: 1
      },
      {
        kind: 'rule', timestamp: now - 1800, last_timestamp: now - 1800, rule_id: 'preview-rule-schedule',
        rule_name: 'Sample weekday status note', fired: true,
        trigger_data: { schedule: '09:00', timezone: 'UTC' }
      }
    ],
    variables: [
      { name: 'sample_count', value: 7, initial: 0, is_counter: true, persistent: false },
      { name: 'preview_mode', value: 'sample', initial: 'sample', is_counter: false, persistent: true }
    ],
    settings: {
      engine: { timezone: 'UTC', alert_stream_enabled: true },
      proxy: { enabled: false, address: '' },
      smtp: { enabled: false, host: '', port: 587, from: '', to: '' },
      mqtt: { enabled: false, host: 'broker.preview.invalid', port: 1883 },
      sparkplug: { enabled: false, group_id: 'sample-group', edge_node_id: 'sample-node' },
      gps: { enabled: false }
    }
  };

  const fakeStatus = {
    engine_version: '1.10.5',
    rules_total: state.rules.length,
    rules_enabled: state.rules.filter(rule => rule.enabled).length,
    events_today: 4,
    device: {
      model: 'Fictional Camera Preview',
      serial: 'SAMPLE-0000',
      ip: '192.0.2.42',
      firmware: '13.0-preview'
    },
    mqtt: { enabled: false, connected: false },
    sparkplug: { enabled: false, connected: false },
    stream: { enabled: true }
  };

  const preview = window.__offlinePreview = {
    requests: [],
    blockedExternalRequests: [],
    state
  };

  function json(value, status = 200) {
    return new Response(JSON.stringify(value), {
      status,
      headers: { 'Content-Type': 'application/json' }
    });
  }

  function text(value = '', status = 200, contentType = 'text/plain') {
    return new Response(value, { status, headers: { 'Content-Type': contentType } });
  }

  function copy(value) {
    return JSON.parse(JSON.stringify(value));
  }

  function getRuleId(url) {
    return url.searchParams.get('id');
  }

  function asJsonBody(init) {
    if (!init.body) return {};
    try { return JSON.parse(init.body); } catch (_) { return {}; }
  }

  function updateObject(target, patch) {
    Object.keys(patch || {}).forEach(key => {
      if (patch[key] && typeof patch[key] === 'object' && !Array.isArray(patch[key])) {
        target[key] = Object.assign(target[key] || {}, patch[key]);
      } else {
        target[key] = patch[key];
      }
    });
    return target;
  }

  function fakeEvents(url) {
    const ruleId = url.searchParams.get('rule');
    let events = state.events;
    if (ruleId) events = events.filter(event => event.rule_id === ruleId);
    if (url.searchParams.get('system') === 'only') events = events.filter(event => event.kind === 'system');
    return copy(events);
  }

  function apiResponse(url, method, init) {
    const path = url.pathname.replace(/^\/local\/acap_event_engine\/?/, '');
    const id = getRuleId(url);
    const body = asJsonBody(init);

    if (path === 'rules') {
      if (method === 'GET' && url.searchParams.get('action') === 'export') return json(copy(state.rules));
      if (method === 'GET') return json(copy(id ? state.rules.filter(rule => rule.id === id) : state.rules));
      if (method === 'DELETE') {
        state.rules = state.rules.filter(rule => rule.id !== id);
        fakeStatus.rules_total = state.rules.length;
        fakeStatus.rules_enabled = state.rules.filter(rule => rule.enabled).length;
        return json({ ok: true, previewOnly: true });
      }
      if (method === 'POST') {
        if (url.searchParams.get('action') === 'import') return json({ imported: 0, previewOnly: true });
        const existing = id && state.rules.find(rule => rule.id === id);
        const next = Object.assign(existing || {}, body);
        if (!next.id) next.id = `preview-rule-${state.rules.length + 1}`;
        if (existing) Object.assign(existing, next);
        else state.rules.push(next);
        fakeStatus.rules_total = state.rules.length;
        fakeStatus.rules_enabled = state.rules.filter(rule => rule.enabled).length;
        return json({ ok: true, id: next.id, previewOnly: true });
      }
    }

    if (path === 'events') {
      if (method === 'DELETE') {
        state.events = [];
        return json({ ok: true, previewOnly: true });
      }
      return json(fakeEvents(url));
    }

    if (path === 'engine') return json(copy(fakeStatus));

    if (path === 'variables') {
      if (method === 'GET' && url.searchParams.has('detail')) return json(copy(state.variables));
      if (method === 'GET') {
        return json(Object.fromEntries(state.variables.map(item => [
          item.name,
          { value: item.value, is_counter: item.is_counter }
        ])));
      }
      if (method === 'DELETE') {
        state.variables = state.variables.filter(item => item.name !== url.searchParams.get('name'));
        return json({ ok: true, previewOnly: true });
      }
      if (method === 'POST') {
        const item = state.variables.find(variable => variable.name === body.name);
        if (item) updateObject(item, body);
        else state.variables.push(Object.assign({ value: '' }, body));
        return json({ ok: true, previewOnly: true });
      }
    }

    if (path === 'settings') {
      if (method === 'GET') return json(copy(state.settings));
      if (method === 'POST') {
        updateObject(state.settings, body);
        return json({ ok: true, previewOnly: true });
      }
    }

    if (path === 'sparkplug') return json({ enabled: false, connected: false, metrics: [] });
    if (path === 'gps') return json({ enabled: false, has_fix: false, fresh: false, satellites: 0 });
    if (path === 'acapevents') return json([]);
    if (path === 'certs') return json([]);
    if (path === 'actions') return json({ ok: true, result: 'Preview action simulated; nothing was sent.', previewOnly: true });
    if (path === 'fire') {
      const rule = state.rules.find(item => item.id === body.id);
      if (rule) {
        state.events.unshift({
          kind: 'rule', timestamp: Math.floor(Date.now() / 1000), rule_id: rule.id,
          rule_name: rule.name, fired: true, trigger_data: { preview: 'simulated in memory' }
        });
      }
      return json({ ok: true, fired: !!rule, previewOnly: true });
    }
    if (path === 'import') return json({ ok: true, previewOnly: true });
    return json({});
  }

  window.fetch = async (input, init = {}) => {
    const address = typeof input === 'string' || input instanceof URL ? String(input) : input.url;
    const url = new URL(address, window.location.href);
    const method = (init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();

    if (url.origin !== window.location.origin) {
      preview.blockedExternalRequests.push(url.href);
      if (url.hostname === 'api.github.com') {
        return json({ tag_name: 'v1.10.5', html_url: 'https://example.invalid/preview-release' });
      }
      return text('External requests are disabled in this offline preview.', 451);
    }

    preview.requests.push({ method, path: url.pathname });
    if (url.pathname.startsWith('/local/acap_event_engine')) return apiResponse(url, method, init);
    if (url.pathname === '/axis-cgi/param.cgi') return text('');
    if (url.pathname === '/axis-cgi/applications/list.cgi') return text('<root/>', 200, 'application/xml');
    if (url.pathname === '/axis-cgi/com/ptz.cgi') return text('preset=Sample preset\n', 200, 'text/plain');
    if (url.pathname === '/axis-cgi/param.cgi') return text('');
    if (url.pathname.startsWith('/axis-cgi/') || url.pathname.startsWith('/vapix/') || url.pathname.startsWith('/config/')) {
      return text('');
    }
    return text('Not found in offline preview.', 404);
  };

  const notice = document.createElement('div');
  notice.textContent = 'OFFLINE PREVIEW · FICTIONAL RULES & DEVICE DATA · ACTIONS ARE SIMULATED IN MEMORY';
  notice.setAttribute('role', 'note');
  notice.style.cssText = 'background:#fff3cd;color:#5a4300;border:1px solid #e8cf83;border-radius:6px;padding:8px 12px;margin:10px auto;max-width:1180px;font:600 11px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;letter-spacing:.03em;text-align:center;';
  document.body.prepend(notice);

  document.addEventListener('click', event => {
    const target = event.target;
    const link = target.closest && target.closest('a[href]');
    const fileInput = target.closest && target.closest('input[type="file"]');
    const copyAction = target.closest && target.closest('[onclick*="copyRuleId"]');
    const blockedLink = link && (
      link.hasAttribute('download') ||
      link.hasAttribute('data-preview-disabled') ||
      new URL(link.href, window.location.href).origin !== window.location.origin
    );
    if (blockedLink || fileInput || copyAction) {
      event.preventDefault();
      event.stopImmediatePropagation();
    }
  }, true);
})();
