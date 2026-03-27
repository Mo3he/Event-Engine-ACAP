'use strict';

/* ===================================================
 * API Layer
 * =================================================== */
const BASE = '/local/acap_event_engine';

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
  testAction:    (type, config) => API.post('actions', { type, config }),

  getEvents:     (limit, rule) => API.get(`events?limit=${limit || 100}${rule ? '&rule=' + rule : ''}`),
  clearEvents:   ()      => API.delete('events'),
  getStatus:     ()      => API.get('engine'),
  getVariables:  ()      => API.get('variables'),
  setVariable:   (name, value, is_counter) => API.post('variables', { name, value, is_counter: !!is_counter }),
  deleteVariable: name   => API.delete(`variables?name=${name}`),
};
