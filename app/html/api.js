'use strict';

/* ===================================================
 * API Layer
 * =================================================== */
const BASE = '/local/acap_event_engine';

async function apiFetch(url, opts) {
  const r = await fetch(url, opts);
  if (!r.ok) {
    let msg = `HTTP ${r.status}`;
    try { const t = await r.text(); if (t) msg += `: ${t.slice(0, 200)}`; } catch(_) {}
    throw new Error(msg);
  }
  return r;
}

const API = {
  get:  path => apiFetch(`${BASE}/${path}`).then(r => r.json()),
  post: (path, body) => apiFetch(`${BASE}/${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  }),
  put: (path, body) => apiFetch(`${BASE}/${path}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  }),
  delete: path => apiFetch(`${BASE}/${path}`, { method: 'DELETE' }),

  getRules:      ()      => API.get('rules'),
  getRule:       id      => API.get(`rules?id=${encodeURIComponent(id)}`),
  exportRules:   ()      => API.get('rules?action=export'),
  importRules:   rules   => API.post('rules?action=import', rules).then(r => r.json()),
  addRule:       rule    => API.post('rules', rule),
  updateRule:    (id, r) => API.post(`rules?id=${encodeURIComponent(id)}`, r),
  deleteRule:    id      => API.delete(`rules?id=${encodeURIComponent(id)}`),
  setEnabled:    (id, v) => API.post(`rules?id=${encodeURIComponent(id)}`, { enabled: v }),
  fireRule:      id      => API.post('fire', { id }),
  testAction:    (type, config) => API.post('actions', { type, config }),

  getEvents:     (limit, rule) => API.get(`events?limit=${limit || 100}${rule ? '&rule=' + rule : ''}`),
  clearEvents:   ()      => API.delete('events'),
  getStatus:     ()      => API.get('engine'),
  getVariables:  ()      => API.get('variables'),
  setVariable:   (name, value, is_counter) => API.post('variables', { name, value, is_counter: !!is_counter }),
  deleteVariable: name   => API.delete(`variables?name=${encodeURIComponent(name)}`),
};
