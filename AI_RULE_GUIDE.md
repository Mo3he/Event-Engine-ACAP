# AI Guide: Creating & Uploading Rules for ACAP Event Engine

This document is a complete reference for an AI to programmatically create rules for the Axis ACAP Event Engine and upload them to a camera. It covers the rule JSON schema, every trigger/condition/action type with their fields, the REST API, template variables, and worked examples.

---

## Table of Contents

1. [Overview](#overview)
2. [API Reference](#api-reference)
3. [Rule JSON Schema](#rule-json-schema)
4. [Trigger Types](#trigger-types)
5. [Condition Types](#condition-types)
6. [Action Types](#action-types)
7. [Template Variables](#template-variables)
8. [Settings & MQTT Configuration](#settings--mqtt-configuration)
9. [Worked Examples](#worked-examples)
10. [Common Patterns](#common-patterns)
11. [Important Notes & Constraints](#important-notes--constraints)

---

## Overview

The ACAP Event Engine is an **If-This-Then-That** style rule engine running on Axis cameras. Rules consist of:

- **Triggers** — events that start evaluation (device events, webhooks, schedules, MQTT messages, I/O, counters, or rule chains)
- **Conditions** — optional gates that must pass before actions execute (time windows, I/O state, counters, variables, HTTP checks, event state, occupancy, day/night)
- **Actions** — what to do when the rule fires (HTTP requests, MQTT publish, recording, PTZ, overlays, audio, email, Slack, Teams, Telegram, InfluxDB, and many more)

All API endpoints are at `https://<camera_ip>/local/acap_event_engine/<endpoint>` and require HTTP digest authentication with admin credentials.

---

## API Reference

### Base URL
```
https://<camera_ip>/local/acap_event_engine
```

### Authentication
HTTP Digest Auth (admin credentials). With curl, use `-u root:pass --anyauth`.

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/rules` | List all rules |
| `GET` | `/rules?id=UUID` | Get a single rule |
| `POST` | `/rules` | Create a new rule (body = rule JSON) |
| `POST` | `/rules?id=UUID` | Update an existing rule |
| `DELETE` | `/rules?id=UUID` | Delete a rule |
| `POST` | `/rules?action=import` | Bulk import rules (body = JSON array of rules) |
| `GET` | `/rules?action=export` | Export all rules as JSON |
| `POST` | `/fire` | Fire a rule: `{"id":"UUID"}` or webhook: `{"token":"...", "payload":{...}}` |
| `GET` | `/engine` | Get engine status (uptime, rule counts, MQTT status, device info) |
| `GET` | `/events` | Get event log (optional `?limit=N&rule=UUID`) |
| `GET` | `/variables` | List all variables and counters |
| `POST` | `/variables` | Set a variable: `{"name":"x","value":"y"}` or counter: `{"name":"x","value":"0","is_counter":true}` |
| `DELETE` | `/variables?name=x` | Delete a variable/counter |
| `GET` | `/settings` | Get current settings |
| `POST` | `/settings` | Update settings (partial update supported) |
| `GET` | `/triggers` | List available trigger types |
| `GET` | `/actions` | List available action types |
| `POST` | `/remote-caps` | Query capabilities of a remote Axis device |

### Create a Rule (curl)
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '@rule.json' \
  "https://CAMERA_IP/local/acap_event_engine/rules"
```

Response: `{"id":"<uuid>","status":"created"}`

### Update a Rule (curl)
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '@rule.json' \
  "https://CAMERA_IP/local/acap_event_engine/rules?id=RULE_UUID"
```

### Bulk Import (curl)
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '[{...rule1...}, {...rule2...}]' \
  "https://CAMERA_IP/local/acap_event_engine/rules?action=import"
```

### Fire via Webhook (curl)
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '{"token":"my-secret","payload":{"key":"value"}}' \
  "https://CAMERA_IP/local/acap_event_engine/fire"
```

### Python Upload Example
```python
import json, urllib.request, ssl

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

BASE = "https://CAMERA_IP"
pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
pm.add_password(None, BASE, "root", "pass")
opener = urllib.request.build_opener(
    urllib.request.HTTPSHandler(context=ctx),
    urllib.request.HTTPDigestAuthHandler(pm),
    urllib.request.HTTPBasicAuthHandler(pm),
)

rule = {
    "name": "My Rule",
    "enabled": True,
    "triggers": [...],
    "conditions": [],
    "actions": [...]
}

body = json.dumps(rule).encode()
url = f"{BASE}/local/acap_event_engine/rules"
req = urllib.request.Request(url, data=body, method="POST",
                            headers={"Content-Type": "application/json"})
resp = opener.open(req)
result = json.loads(resp.read().decode())
print(result)  # {"id": "...", "status": "created"}
```

---

## Rule JSON Schema

```json
{
  "name": "Rule Name (max 100 chars)",
  "description": "Optional description",
  "enabled": true,
  "trigger_logic": "OR",
  "trigger_window": 0,
  "condition_logic": "AND",
  "cooldown": 0,
  "max_executions": 0,
  "max_exec_period": "",
  "triggers": [ ... ],
  "conditions": [ ... ],
  "actions": [ ... ]
}
```

### Top-Level Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | **required** | Rule name (max 100 characters) |
| `description` | string | `""` | Optional description |
| `enabled` | boolean | `true` | Whether the rule is active |
| `trigger_logic` | `"OR"` \| `"AND"` \| `"AND_ACTIVE"` | `"OR"` | How multiple triggers combine. `OR` = any trigger fires the rule. `AND` = all must fire within `trigger_window`. `AND_ACTIVE` = all must be in active state simultaneously. |
| `trigger_window` | integer | `0` | For `AND` logic: seconds within which all triggers must fire. `0` = no time limit. |
| `condition_logic` | `"AND"` \| `"OR"` | `"AND"` | How multiple conditions combine |
| `cooldown` | integer | `0` | Minimum seconds between consecutive firings (0 = no cooldown) |
| `max_executions` | integer | `0` | Maximum times rule can fire per period (0 = unlimited) |
| `max_exec_period` | `""` \| `"minute"` \| `"hour"` \| `"day"` \| `"lifetime"` | `""` | Period for `max_executions`. Empty = lifetime total. |
| `triggers` | array | **required** | At least one trigger |
| `conditions` | array | `[]` | Optional conditions (empty = always pass) |
| `actions` | array | **required** | At least one action |

### Read-Only Fields (returned by the API, do not set when creating)

| Field | Type | Description |
|-------|------|-------------|
| `id` | UUID string | Auto-assigned on creation |
| `execution_count` | integer | Total times fired |
| `last_fired` | ISO 8601 datetime | Last fire timestamp |

---

## Trigger Types

### 1. `vapix_event` — Device Event

Fires on any matching Axis device event (motion detection, analytics, sensors, I/O, etc.).

```json
{
  "type": "vapix_event",
  "topic0": { "tns1": "RuleEngine" },
  "topic1": { "tnsaxis": "MotionDetection" },
  "topic2": {},
  "topic3": {},
  "filter_key": "active",
  "filter_value": true,
  "value_key": "Temperature",
  "value_op": "gt",
  "value_threshold": 30,
  "value_threshold2": 50,
  "value_hold_secs": 10
}
```

| Field | Type | Description |
|-------|------|-------------|
| `topic0` | object | Top-level topic `{namespace: value}`, e.g. `{"tns1": "RuleEngine"}` |
| `topic1` | object | Second-level topic, e.g. `{"tnsaxis": "MotionDetection"}` |
| `topic2` | object | Third-level topic (optional) |
| `topic3` | object | Fourth-level topic (optional) |
| `filter_key` | string | Event data key for boolean filtering |
| `filter_value` | boolean | Required value for `filter_key` |
| `value_key` | string | Event data key for numeric threshold |
| `value_op` | `"gt"` \| `"lt"` \| `"eq"` \| `"between"` | Comparison operator |
| `value_threshold` | number | Threshold value |
| `value_threshold2` | number | Upper bound for `"between"` |
| `value_hold_secs` | integer | Condition must hold this many seconds before firing (0 = immediate) |

**Common topic patterns:**
- Motion Detection: `topic0: {"tns1": "RuleEngine"}, topic1: {"tnsaxis": "MotionDetection"}`
- Object Analytics (ACAP): `topic0: {"tnsaxis": "CameraApplicationPlatform"}, topic1: {"tnsaxis": "ObjectAnalytics"}, topic2: {"tnsaxis": "Device1Scenario1"}`
- I/O Port: `topic0: {"tns1": "Device"}, topic1: {"tnsaxis": "IO"}, topic2: {"tnsaxis": "Port"}`
- Virtual Input: `topic0: {"tns1": "Device"}, topic1: {"tnsaxis": "IO"}, topic2: {"tnsaxis": "VirtualInput"}`
- Day/Night: `topic0: {"tns1": "VideoSource"}, topic1: {"tnsaxis": "DayNightVision"}`
- Audio Detection: `topic0: {"tns1": "AudioSource"}, topic1: {"tnsaxis": "TriggerLevel"}`
- Temperature: `topic0: {"tns1": "Device"}, topic1: {"tnsaxis": "Sensor"}, topic2: {"tnsaxis": "Temperature"}`
- Air Quality: `topic0: {"tns1": "Device"}, topic1: {"tnsaxis": "AirQuality"}`
- Tampering: `topic0: {"tns1": "RuleEngine"}, topic1: {"tnsaxis": "Tampering"}`
- Storage Disruption: `topic0: {"tns1": "Device"}, topic1: {"tnsaxis": "Storage"}, topic2: {"tnsaxis": "Disruption"}`
- PIR Sensor: `topic0: {"tns1": "Device"}, topic1: {"tnsaxis": "Sensor"}, topic2: {"tnsaxis": "PIR"}`

### 2. `http_webhook` — HTTP Webhook

Fires when POST `/fire` is called with a matching token.

```json
{
  "type": "http_webhook",
  "token": "my-secret-token"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `token` | string | Secret token to match (max 120 chars). Matched from `body.token` or `?token=` query param. |

Payload data from the webhook POST body's `payload` field is available as `{{trigger.KEY}}` in actions.

### 3. `schedule` — Schedule

Fires on a time-based schedule. Four sub-types:

**Cron:**
```json
{
  "type": "schedule",
  "schedule_type": "cron",
  "cron": "0 8 * * 1-5"
}
```

**Interval:**
```json
{
  "type": "schedule",
  "schedule_type": "interval",
  "interval_seconds": 300
}
```

**Daily Time:**
```json
{
  "type": "schedule",
  "schedule_type": "daily_time",
  "time": "08:00",
  "days": [1, 2, 3, 4, 5]
}
```

**Astronomical (sunrise/sunset):**
```json
{
  "type": "schedule",
  "schedule_type": "astronomical",
  "event": "sunset",
  "latitude": 59.3293,
  "longitude": 18.0686,
  "offset_minutes": -30
}
```

| Field | Type | Description |
|-------|------|-------------|
| `schedule_type` | `"cron"` \| `"interval"` \| `"daily_time"` \| `"astronomical"` | Sub-type |
| `cron` | string | Cron expression: `min hour dom mon dow` |
| `interval_seconds` | integer | Seconds between firings |
| `time` | string | Time in `HH:MM` format |
| `days` | array of int | Days of week: 0=Sun, 1=Mon, …, 6=Sat |
| `event` | `"sunrise"` \| `"sunset"` \| `"dawn"` \| `"dusk"` \| `"solar_noon"` | Astronomical event |
| `latitude` | number | Decimal degrees (North positive). Defaults to engine setting. |
| `longitude` | number | Decimal degrees (East positive). Defaults to engine setting. |
| `offset_minutes` | integer | Offset from astronomical event (negative = before, positive = after) |

### 4. `mqtt_message` — MQTT Message

Fires when a matching MQTT message is received (requires MQTT to be configured and connected).

```json
{
  "type": "mqtt_message",
  "topic_filter": "sensors/+/temperature",
  "payload_filter": "alert"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `topic_filter` | string | MQTT topic filter. Supports `+` (single-level) and `#` (multi-level) wildcards. |
| `payload_filter` | string | Optional substring that must appear in the payload |

The entire MQTT payload is available as trigger data. If the payload is JSON, individual fields are accessible as `{{trigger.KEY}}`.

### 5. `io_input` — I/O Input

Fires on a digital I/O input port state change.

```json
{
  "type": "io_input",
  "port": 1,
  "edge": "rising",
  "hold_secs": 0
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `port` | integer | — | I/O port number (1-based) |
| `edge` | `"rising"` \| `"falling"` \| `"both"` \| `"cut"` \| `"short"` | `"rising"` | Which edge(s) to fire on. `cut` = open-circuit/wire-cut fault; `short` = short-circuit fault (supervised inputs only) |
| `hold_secs` | integer | `0` | Port must remain in triggered state this many seconds before firing |

### 6. `counter_threshold` — Counter Threshold

Fires when a named counter crosses a threshold value. Checked every 1 second.

```json
{
  "type": "counter_threshold",
  "counter_name": "motion_count",
  "threshold": 10,
  "op": "gte"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `counter_name` | string | — | Name of counter to watch |
| `threshold` | number | — | Value to compare against |
| `op` | `"gte"` \| `"lte"` \| `"eq"` | `"gte"` | Comparison: ≥, ≤, = |

### 7. `rule_fired` — Rule Chain

Fires whenever another specified rule executes successfully.

```json
{
  "type": "rule_fired",
  "rule_id": "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `rule_id` | UUID string | ID of the rule whose execution triggers this rule |

### 8. `aoa_scenario` — AOA Object Analytics Scenario

Fires when an object is detected in an AXIS Object Analytics (AOA) scenario.

```json
{
  "type": "aoa_scenario",
  "scenario_id": 1,
  "object_class": "human"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `scenario_id` | integer | AOA scenario number (1-based) |
| `object_class` | string | Optional filter: `"human"`, `"car"`, `"truck"`, `"bus"`, `"bike"`, etc. Empty = all objects. |

---

## Condition Types

Conditions are optional. If the `conditions` array is empty, the rule always passes. Multiple conditions are combined with `condition_logic` (`"AND"` or `"OR"`).

### 1. `time_window` — Time of Day

```json
{
  "type": "time_window",
  "start": "08:00",
  "end": "18:00",
  "days": [1, 2, 3, 4, 5]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `start` | string | Start time `HH:MM` |
| `end` | string | End time `HH:MM`. Supports wraparound (e.g. `"22:00"` to `"06:00"` means overnight). |
| `days` | array of int | Optional. Days of week: 0=Sun, 1=Mon, …, 6=Sat. Omit = every day. |

### 2. `io_state` — I/O Port State

```json
{
  "type": "io_state",
  "port": 1,
  "state": "active"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `port` | integer | I/O port number (1-based) |
| `state` | `"active"` \| `"inactive"` | Required port state |

### 3. `counter` — Counter Comparison

```json
{
  "type": "counter",
  "counter_name": "motion_count",
  "op": "gte",
  "threshold": 5
}
```

| Field | Type | Description |
|-------|------|-------------|
| `counter_name` | string | Counter name |
| `op` | `"gt"` \| `"lt"` \| `"eq"` \| `"gte"` \| `"lte"` | Comparison operator |
| `threshold` | number | Value to compare against |

### 4. `variable_compare` — Variable Comparison

```json
{
  "type": "variable_compare",
  "name": "system.armed",
  "op": "eq",
  "value": "true"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | — | Variable name |
| `op` | `"eq"` \| `"neq"` \| `"gt"` \| `"lt"` | `"eq"` | Comparison operator |
| `value` | string | — | Value to compare against (string comparison; `"gt"` and `"lt"` are numeric) |

### 5. `http_check` — HTTP Response Check

```json
{
  "type": "http_check",
  "url": "http://example.com/status",
  "expected_status": 200,
  "expected_body": "ok",
  "json_path": "data.status",
  "json_expected": "armed"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `url` | string | URL to fetch |
| `expected_status` | integer | Expected HTTP status code (default: 200) |
| `expected_body` | string | Substring that must appear in response body |
| `json_path` | string | Dot-notation path into JSON response (e.g. `"data.temperature"`) |
| `json_expected` | string | Expected value at `json_path` |

### 6. `vapix_event_state` — Live Event State Check

Checks the current state of a VAPIX event instance on the local or a remote device.

```json
{
  "type": "vapix_event_state",
  "event_key": "tnsaxis:AirQuality",
  "data_key": "CO2",
  "op": "gt",
  "threshold": 800,
  "remote_host": "192.168.1.100",
  "remote_user": "root",
  "remote_pass": "pass"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `event_key` | string | Partial topic string to match |
| `data_key` | string | Data field name to check |
| `op` | `"boolean"` \| `"gt"` \| `"lt"` \| `"gte"` \| `"lte"` \| `"eq"` \| `"between"` \| `"contains"` \| `"eq_str"` | Comparison operator |
| `expected` | string | For boolean (`"true"`/`"false"`), contains (substring), or exact match |
| `threshold` | number | For numeric comparisons |
| `threshold2` | number | Upper bound for `"between"` |
| `remote_host` | string | Optional: IP of remote Axis device |
| `remote_user` | string | Username for remote device |
| `remote_pass` | string | Password for remote device |

### 7. `day_night` — Day/Night Check

```json
{
  "type": "day_night",
  "state": "day",
  "lat": 59.3293,
  "lon": 18.0686
}
```

| Field | Type | Description |
|-------|------|-------------|
| `state` | `"day"` \| `"night"` | Required state |
| `lat` | number | Latitude (optional, defaults to engine setting) |
| `lon` | number | Longitude (optional, defaults to engine setting) |

### 8. `aoa_occupancy` — AOA Occupancy Check

Queries AXIS Object Analytics occupancy count for a scenario.

```json
{
  "type": "aoa_occupancy",
  "scenario_id": 1,
  "object_class": "human",
  "op": "gte",
  "value": 3,
  "remote_host": "192.168.1.100",
  "remote_user": "root",
  "remote_pass": "pass"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `scenario_id` | integer | AOA scenario number |
| `object_class` | string | `"human"`, `"car"`, `"any"`, etc. `"any"` or omit = total count. |
| `op` | `"gt"` \| `"gte"` \| `"lt"` \| `"lte"` \| `"eq"` | Comparison operator |
| `value` | number | Threshold to compare against |
| `remote_host` | string | Optional: IP of remote device |
| `remote_user` | string | Username for remote device |
| `remote_pass` | string | Password for remote device |

---

## Action Types

Actions execute sequentially. Use `"delay"` to insert pauses between actions.

### Remote Device Support

Many actions support targeting a remote Axis device. Add these fields to any supported action:

```json
{
  "remote_host": "192.168.1.100",
  "remote_user": "root",
  "remote_pass": "pass"
}
```

Supported by: `recording`, `overlay_text`, `ptz_preset`, `io_output`, `audio_clip`, `siren_light`, `guard_tour`, `set_device_param`, `ir_cut_filter`, `privacy_mask`, `wiper`, `light_control`, `acap_control`, `speaker_display`, `paging_console_execute`, `paging_console_button`.

---

### 1. `http_request` — HTTP Request

```json
{
  "type": "http_request",
  "url": "http://example.com/notify",
  "method": "POST",
  "headers": "Content-Type: application/json\nX-Api-Key: secret",
  "body": "{\"ts\":\"{{timestamp}}\",\"serial\":\"{{camera.serial}}\"}",
  "username": "admin",
  "password": "pass",
  "attach_snapshot": false,
  "on_failure": [
    { "type": "send_syslog", "message": "HTTP request failed", "level": "error" }
  ]
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `url` | string | — | Destination URL. Supports template variables. |
| `method` | `"GET"` \| `"POST"` \| `"PUT"` \| `"DELETE"` | `"GET"` | HTTP method |
| `headers` | string | `""` | Headers, one per line: `Key: Value` |
| `body` | string | `""` | Request body. Supports template variables. |
| `username` | string | — | HTTP Basic/Digest auth username |
| `password` | string | — | HTTP Basic/Digest auth password |
| `attach_snapshot` | boolean | `false` | Capture JPEG snapshot, available as `{{trigger.snapshot_base64}}` |
| `on_failure` | array | — | Actions to execute if request fails (non-2xx or network error) |

### 2. `mqtt_publish` — MQTT Publish

```json
{
  "type": "mqtt_publish",
  "topic": "cameras/{{camera.serial}}/events",
  "payload": "{\"ts\":\"{{timestamp}}\",\"event\":\"{{trigger_json}}\"}",
  "qos": 0,
  "retain": false,
  "attach_snapshot": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `topic` | string | — | MQTT topic. Supports template variables. |
| `payload` | string | — | Message payload. Supports template variables. |
| `qos` | `0` \| `1` | `0` | MQTT QoS level |
| `retain` | boolean | `false` | Retain message on broker |
| `attach_snapshot` | boolean | `false` | Makes `{{trigger.snapshot_base64}}` available |

### 3. `recording` — Start/Stop Recording

```json
{
  "type": "recording",
  "operation": "start",
  "duration": 30,
  "while_active": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `operation` | `"start"` \| `"stop"` | `"start"` | Start or stop recording |
| `duration` | integer | `0` | Max recording duration in seconds (0 = unlimited) |
| `while_active` | boolean | `false` | Auto-stop when triggering event goes inactive |

### 4. `overlay_text` — Dynamic Overlay Text

```json
{
  "type": "overlay_text",
  "text": "Motion at {{time}}",
  "duration": 10,
  "channel": 1,
  "position": "topLeft",
  "text_color": "white",
  "while_active": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `text` | string | — | Overlay text. Supports template variables. |
| `duration` | integer | `0` | Seconds to display (0 = until next update) |
| `channel` | integer | `1` | Video channel |
| `position` | string | `topLeft` | `keep` (leave current position untouched), `topLeft`, `topRight`, `bottomLeft`, `bottomRight`, `top` (top centre), `bottom` (bottom centre), or `custom` |
| `pos_x` | number | `-0.99` | Custom X coordinate (-1..1, left to right). Used when `position` = `custom` |
| `pos_y` | number | `-0.99` | Custom Y coordinate (-1..1, top to bottom). Used when `position` = `custom` |
| `text_color` | string | `white` | `white`, `black`, `red`, `transparent`, `semiTransparent` |
| `while_active` | boolean | `false` | Auto-clear when trigger goes inactive |

### 5. `ptz_preset` — PTZ Preset

```json
{
  "type": "ptz_preset",
  "preset": "Home",
  "channel": 1
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `preset` | string | — | PTZ preset name |
| `channel` | integer | `1` | Camera channel |

### 6. `io_output` — Digital Output

```json
{
  "type": "io_output",
  "port": 1,
  "state": "active",
  "duration": 5,
  "while_active": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `port` | integer | — | I/O port number (1-based) |
| `state` | `"active"` \| `"inactive"` | — | Output state |
| `duration` | integer | `0` | Seconds to hold, then revert (0 = permanent) |
| `while_active` | boolean | `false` | Auto-revert when trigger goes inactive |

### 7. `audio_clip` — Play Audio Clip

```json
{
  "type": "audio_clip",
  "clip_name": "alarm.wav",
  "volume": 80,
  "loop_count": 1,
  "channel": 1,
  "while_active": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `clip_name` | string | — | Name of audio clip as uploaded to the camera |
| `volume` | integer 0–100 | `70` | Playback volume |
| `loop_count` | integer | `1` | Number of times to play. Set to `0` with `while_active: true` to loop indefinitely |
| `channel` | integer | `1` | Audio output channel |
| `while_active` | boolean | `false` | Loop indefinitely and auto-stop when trigger deactivates. Requires `loop_count: 0` |

### 8. `siren_light` — Siren/Light Profile

For supported devices (e.g. Axis siren-strobe units).

```json
{
  "type": "siren_light",
  "profile": "Intrusion",
  "signal_action": "start",
  "while_active": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `profile` | string | — | Siren/light profile name |
| `signal_action` | `"start"` \| `"stop"` | `"start"` | Start or stop the signal |
| `while_active` | boolean | `false` | Auto-stop when trigger goes inactive |

### 9. `send_syslog` — System Log Message

```json
{
  "type": "send_syslog",
  "message": "Rule fired at {{timestamp}}",
  "level": "info"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `message` | string | — | Syslog message. Supports template variables. |
| `level` | `"info"` \| `"warning"` \| `"error"` | `"info"` | Log level |

### 10. `fire_vapix_event` — Fire ACAP Event

```json
{
  "type": "fire_vapix_event",
  "event_id": "RuleFired",
  "state": true
}
```

| Field | Type | Description |
|-------|------|-------------|
| `event_id` | string | Event ID: `"RuleFired"`, `"RuleError"`, `"EngineReady"`, or custom |
| `state` | boolean | For stateful events: `true` = High, `false` = Low. Omit for pulse events. |

### 11. `delay` — Pause

```json
{
  "type": "delay",
  "seconds": 5
}
```

| Field | Type | Description |
|-------|------|-------------|
| `seconds` | integer | Seconds to wait (minimum 1). Execution is async — does not block other rules. |

### 12. `set_variable` — Set Variable

```json
{
  "type": "set_variable",
  "name": "system.armed",
  "value": "true"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Variable name |
| `value` | string | Value to set. Supports template variables. |

### 13. `increment_counter` — Increment/Decrement Counter

```json
{
  "type": "increment_counter",
  "name": "motion_count",
  "delta": 1
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | string | — | Counter name |
| `delta` | number | `1` | Amount to add (positive) or subtract (negative) |

### 14. `run_rule` — Trigger Another Rule

```json
{
  "type": "run_rule",
  "rule_id": "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `rule_id` | UUID string | ID of the rule to trigger. Skipped if target rule is disabled. |

### 15. `set_rule_enabled` — Enable/Disable Another Rule

```json
{
  "type": "set_rule_enabled",
  "rule_id": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "enabled": true
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `rule_id` | UUID string | — | ID of the rule to modify |
| `enabled` | boolean | `true` | Whether to enable or disable |

### 16. `vapix_query` — Device Event Query

Fetches the latest cached data from a device event and injects values as `{{trigger.FIELD}}` for subsequent actions.

```json
{
  "type": "vapix_query",
  "topic0": { "tns1": "Device" },
  "topic1": { "tnsaxis": "AirQuality" },
  "remote_host": "192.168.1.100",
  "remote_user": "root",
  "remote_pass": "pass"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `topic0`–`topic3` | object | Topic filters matching the event to query |
| `remote_host` | string | Optional: query a remote device instead of local cache |
| `remote_user` | string | Remote device username |
| `remote_pass` | string | Remote device password |

### 17. `guard_tour` — PTZ Guard Tour

```json
{
  "type": "guard_tour",
  "operation": "start",
  "tour_id": "Guard Tour 1",
  "channel": 1
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `operation` | `"start"` \| `"stop"` | `"start"` | Start or stop the tour |
| `tour_id` | string | — | Guard tour ID or name |
| `channel` | integer | `1` | Camera channel (1–8) |

### 18. `set_device_param` — Set Camera Parameter

```json
{
  "type": "set_device_param",
  "parameter": "ImageSource.I0.Sensor.Sharpness",
  "value": "50"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `parameter` | string | Parameter path (e.g. `ImageSource.I0.Sensor.Sharpness`). The `root.` prefix is added automatically. |
| `value` | string | Value to set |

### 19. `snapshot_upload` — Capture & Upload JPEG

```json
{
  "type": "snapshot_upload",
  "url": "https://example.com/upload/{{camera.serial}}/{{timestamp}}.jpg",
  "method": "POST",
  "channel": 1,
  "username": "user",
  "password": "pass",
  "headers": "X-Camera: {{camera.serial}}"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `url` | string | — | Destination URL. Supports template variables. |
| `method` | `"POST"` \| `"PUT"` | `"POST"` | Upload method |
| `channel` | integer | `1` | Video channel |
| `username` | string | — | HTTP auth username |
| `password` | string | — | HTTP auth password |
| `headers` | string | — | Extra headers (Key: Value, one per line) |

### 20. `ir_cut_filter` — Day/Night Mode

```json
{
  "type": "ir_cut_filter",
  "mode": "night",
  "channel": 1
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `mode` | `"auto"` \| `"day"` \| `"night"` | `"auto"` | IR cut filter mode |
| `channel` | integer | `1` | Video channel |

### 21. `privacy_mask` — Privacy Mask Control

```json
{
  "type": "privacy_mask",
  "mask_id": "Privacy Mask 1",
  "enabled": true,
  "channel": 1
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `mask_id` | string | — | Privacy mask ID or name |
| `enabled` | boolean | `true` | Enable or disable the mask |
| `channel` | integer | `1` | Video channel |

### 22. `wiper` — Windshield Wiper / Clear View

```json
{
  "type": "wiper",
  "operation": "start",
  "id": 1,
  "duration": 10
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `operation` | `"start"` \| `"stop"` | `"start"` | Start or stop wiper |
| `id` | integer | `1` | Clear view device ID |
| `duration` | integer | — | Duration in seconds (omit for device default) |

### 23. `light_control` — Illuminator Control

```json
{
  "type": "light_control",
  "operation": "on",
  "light_id": "led0",
  "intensity": 80
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `operation` | `"on"` \| `"off"` \| `"auto"` | `"on"` | on/off/auto mode |
| `light_id` | string | `"led0"` | Light device ID |
| `intensity` | integer | — | 0–100 brightness (only for `"on"`) |

### 24. `acap_control` — Start/Stop ACAP Application

```json
{
  "type": "acap_control",
  "package": "com.axis.myapp",
  "operation": "restart"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `package` | string | — | ACAP package name |
| `operation` | `"start"` \| `"stop"` \| `"restart"` | `"start"` | Operation |

### 25. `speaker_display` — Speaker Display Notification

For Axis speaker-display devices (AXIS OS 11.11+).

```json
{
  "type": "speaker_display",
  "operation": "show",
  "message": "Welcome! Current time: {{time}}",
  "textColor": "#FFFFFF",
  "backgroundColor": "#004400",
  "textSize": "medium",
  "scrollDirection": "fromRightToLeft",
  "scrollSpeed": 5,
  "duration_type": "time",
  "duration_value": 15000,
  "while_active": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `operation` | `"show"` \| `"stop"` | `"show"` | Show a message or stop current display |
| `message` | string | — | Display text. Supports template variables. Max 1000 chars. |
| `textColor` | string | `"#FFFFFF"` | Text color in `#RRGGBB` |
| `backgroundColor` | string | — | Background color in `#RRGGBB` |
| `textSize` | `"small"` \| `"medium"` \| `"large"` | `"medium"` | Font size |
| `scrollDirection` | `"fromRightToLeft"` \| `"fromBottomToTop"` | — | Scroll direction. Omit for static text. |
| `scrollSpeed` | integer | `5` | 0=static, 1=slowest, 10=fastest |
| `duration_type` | `"repetitions"` \| `"time"` \| `"timeCompleteMessage"` | — | Duration mode. Omit for indefinite. |
| `duration_value` | integer | — | Repetitions or milliseconds depending on `duration_type` |
| `while_active` | boolean | `false` | Auto-stop when trigger clears (only when `duration_type` is omitted) |

### 26. `email` — Send Email

Requires SMTP to be configured in settings.

```json
{
  "type": "email",
  "to": "security@example.com",
  "subject": "Alert — {{camera.model}}",
  "body": "Motion detected at {{time}} on {{camera.ip}}",
  "attach_snapshot": true
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `to` | string | — | Recipients (comma/semicolon separated) |
| `subject` | string | `"Event Engine Alert"` | Subject. Supports template variables. |
| `body` | string | — | Body. Supports template variables. |
| `attach_snapshot` | boolean | `false` | Attach JPEG snapshot |

### 27. `telegram` — Telegram Message

```json
{
  "type": "telegram",
  "bot_token": "123456:ABC-DEF",
  "chat_id": "-1001234567890",
  "message": "Alert on {{camera.model}} at {{time}}",
  "parse_mode": "Markdown",
  "attach_snapshot": true,
  "disable_preview": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `bot_token` | string | — | Telegram bot token |
| `chat_id` | string | — | Chat/group/channel ID |
| `message` | string | — | Message text. Supports template variables. |
| `parse_mode` | `"Markdown"` \| `"HTML"` | — | Message formatting |
| `attach_snapshot` | boolean | `false` | Send photo with caption (falls back to text if snapshot fails) |
| `disable_preview` | boolean | `false` | Disable link preview |

### 28. `slack_webhook` — Slack Webhook

```json
{
  "type": "slack_webhook",
  "webhook_url": "https://hooks.slack.com/services/YOUR/SLACK/WEBHOOK",
  "message": ":rotating_light: Motion on {{camera.model}} at {{time}}",
  "channel": "#security-alerts",
  "username": "Axis Camera",
  "attach_snapshot": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `webhook_url` | string | Slack incoming webhook URL |
| `message` | string | Message text. Supports template variables. |
| `channel` | string | Optional channel override |
| `username` | string | Optional username override |
| `attach_snapshot` | boolean | Include snapshot |

### 29. `teams_webhook` — Microsoft Teams Webhook

```json
{
  "type": "teams_webhook",
  "webhook_url": "https://YOUR_TENANT.webhook.office.com/...",
  "title": "Motion Detected",
  "message": "Camera {{camera.serial}} at {{time}}",
  "theme_color": "FF6600",
  "attach_snapshot": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `webhook_url` | string | Teams incoming webhook URL |
| `title` | string | Card title. Supports template variables. |
| `message` | string | Card body. Supports template variables. |
| `theme_color` | string | Hex color for the card accent |
| `attach_snapshot` | boolean | Include snapshot |

### 30. `influxdb_write` — InfluxDB Time Series Write

```json
{
  "type": "influxdb_write",
  "url": "http://influxdb.local:8086",
  "version": "v2",
  "org": "my-org",
  "bucket": "sensors",
  "token": "my-influx-token",
  "measurement": "environment",
  "tags": "camera={{camera.serial}}",
  "fields": "temperature={{trigger.Temperature|2}},humidity={{trigger.Humidity|1}}"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `url` | string | — | InfluxDB base URL |
| `version` | `"v1"` \| `"v2"` | `"v2"` | InfluxDB API version |
| `measurement` | string | — | Measurement name. Supports template variables. |
| `fields` | string | — | Line protocol fields. Supports template variables. |
| `tags` | string | — | Line protocol tags. Supports template variables. |
| **v2:** `org` | string | — | Organization |
| **v2:** `bucket` | string | — | Bucket |
| **v2:** `token` | string | — | Bearer token |
| **v1:** `database` | string | — | Database name |
| **v1:** `username` | string | — | Basic auth user |
| **v1:** `token` | string | — | Basic auth password |

### 31. `ftp_upload` — FTP/SFTP Snapshot Upload

```json
{
  "type": "ftp_upload",
  "url": "ftp://server.example.com/snapshots/{{camera.serial}}_{{timestamp}}.jpg",
  "username": "ftpuser",
  "password": "ftppass"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `url` | string | FTP/SFTP URL (including filename). Supports template variables. |
| `username` | string | FTP credentials |
| `password` | string | FTP credentials |

### 32. `digest` — Notification Digest (Batched Alerts)

Buffers events and periodically flushes a summary via the specified delivery channel.

```json
{
  "type": "digest",
  "deliver_via": "email",
  "interval": 86400,
  "line": "{{time}} — Motion detected on {{camera.model}}",
  "to": "manager@example.com",
  "subject": "Daily Activity Digest — {{camera.serial}}"
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `deliver_via` | `"slack"` \| `"teams"` \| `"email"` \| `"mqtt"` \| `"telegram"` | `"slack"` | Delivery channel |
| `interval` | integer | `300` | Flush interval in seconds (minimum 30) |
| `line` | string | `"{{timestamp}} — {{trigger_json}}"` | Template for each buffered line |
| *(plus all fields of the target channel)* | | | e.g. `webhook_url` for Slack, `to`/`subject` for email, `topic`/`payload` for MQTT |

### 33. `paging_console_execute` — Execute Paging Action

```json
{
  "type": "paging_console_execute",
  "action_id": "UUID-of-paging-action"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `action_id` | UUID string | Paging console action to execute |

### 34. `paging_console_button` — Set Paging Console Button

Assigns or clears a paging console button slot.

```json
{
  "type": "paging_console_button",
  "page_id": "UUID-of-page",
  "slot": 1,
  "action_id": "UUID-of-action-or-empty-to-clear"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `page_id` | UUID string | Paging console page ID |
| `slot` | integer | Button slot number (1–32) |
| `action_id` | string | Action UUID to assign, or empty string to clear the slot |

### 35. `aoa_get_counts` — AOA Accumulated Counts

Fetches AXIS Object Analytics accumulated counts and injects them as trigger data fields.

```json
{
  "type": "aoa_get_counts",
  "scenario_id": 1,
  "reset_after": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `scenario_id` | integer | — | AOA scenario number |
| `reset_after` | boolean | `false` | Reset counts after fetching |

Injected fields: `{{trigger.aoa_total}}`, `{{trigger.aoa_human}}`, `{{trigger.aoa_car}}`, `{{trigger.aoa_truck}}`, `{{trigger.aoa_bus}}`, `{{trigger.aoa_bike}}`, `{{trigger.aoa_otherVehicle}}`, `{{trigger.aoa_timestamp}}`

---

## Template Variables

All string fields in actions support `{{...}}` variable substitution:

| Variable | Value |
|----------|-------|
| `{{timestamp}}` | RFC 3339 timestamp in device local time (e.g. `2026-04-30T13:00:00+02:00`) |
| `{{date}}` | `YYYY-MM-DD` |
| `{{time}}` | `HH:MM:SS` |
| `{{camera.serial}}` | Device serial number |
| `{{camera.model}}` | Device model name |
| `{{camera.ip}}` | Device IP address |
| `{{trigger_json}}` | Full trigger event data as compact JSON |
| `{{trigger.KEY}}` | Individual field from the trigger event data |
| `{{trigger.KEY\|N}}` | Numeric value rounded to N decimals (e.g. `{{trigger.Temperature\|2}}` → `20.35`) |
| `{{trigger.snapshot_base64}}` | Base64 JPEG (when `attach_snapshot` is `true`) |
| `{{var.NAME}}` | Value of a named variable |
| `{{counter.NAME}}` | Value of a named counter |

---

## Settings & MQTT Configuration

### Get Settings
```bash
curl -s -k -u root:pass --anyauth "https://CAMERA_IP/local/acap_event_engine/settings"
```

### Configure MQTT
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '{
    "mqtt": {
      "enabled": true,
      "host": "broker.example.com",
      "port": 1883,
      "client_id": "axis-cam-01",
      "username": "camuser",
      "password": "secret",
      "keepalive": 60,
      "use_tls": false
    }
  }' \
  "https://CAMERA_IP/local/acap_event_engine/settings"
```

### Configure SMTP (for email actions)
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '{
    "smtp": {
      "server": "smtp.example.com:587",
      "username": "alerts@example.com",
      "password": "smtp-password",
      "from": "alerts@example.com",
      "use_tls": true
    }
  }' \
  "https://CAMERA_IP/local/acap_event_engine/settings"
```

### Set Location (for astronomical triggers)
```bash
curl -s -k -u root:pass --anyauth \
  -X POST -H "Content-Type: application/json" \
  -d '{"engine": {"latitude": 59.3293, "longitude": 18.0686}}' \
  "https://CAMERA_IP/local/acap_event_engine/settings"
```

---

## Worked Examples

### Example 1: Motion Detection → Slack Alert with Snapshot

```json
{
  "name": "Motion → Slack Alert",
  "enabled": true,
  "cooldown": 30,
  "triggers": [
    {
      "type": "vapix_event",
      "topic0": { "tns1": "RuleEngine" },
      "topic1": { "tnsaxis": "MotionDetection" },
      "filter_key": "active",
      "filter_value": true
    }
  ],
  "conditions": [
    {
      "type": "time_window",
      "start": "18:00",
      "end": "08:00",
      "days": [0, 1, 2, 3, 4, 5, 6]
    }
  ],
  "actions": [
    {
      "type": "slack_webhook",
      "webhook_url": "https://hooks.slack.com/services/YOUR/SLACK/WEBHOOK",
      "message": ":rotating_light: *Motion Detected*\nCamera: {{camera.model}} ({{camera.serial}})\nTime: {{date}} {{time}}",
      "attach_snapshot": true
    }
  ]
}
```

### Example 2: Scheduled Sensor Data to InfluxDB

```json
{
  "name": "Sensor → InfluxDB Every 60s",
  "enabled": true,
  "triggers": [
    {
      "type": "schedule",
      "schedule_type": "interval",
      "interval_seconds": 60
    }
  ],
  "conditions": [],
  "actions": [
    {
      "type": "vapix_query",
      "topic0": { "tns1": "Device" },
      "topic1": { "tnsaxis": "AirQuality" }
    },
    {
      "type": "influxdb_write",
      "url": "http://influxdb.local:8086",
      "version": "v2",
      "org": "my-org",
      "bucket": "sensors",
      "token": "my-token",
      "measurement": "air_quality",
      "tags": "camera={{camera.serial}}",
      "fields": "co2={{trigger.CO2|0}},pm25={{trigger.PM25|1}},temperature={{trigger.Temperature|1}}"
    }
  ]
}
```

### Example 3: MQTT Command → Arm/Disarm System

**Arm rule:**
```json
{
  "name": "Arm System via MQTT",
  "enabled": true,
  "triggers": [
    {
      "type": "mqtt_message",
      "topic_filter": "cameras/+/control",
      "payload_filter": "arm"
    }
  ],
  "conditions": [],
  "actions": [
    { "type": "set_variable", "name": "system.armed", "value": "true" },
    { "type": "send_syslog", "message": "System ARMED at {{timestamp}}", "level": "info" }
  ]
}
```

**Motion alert (only when armed):**
```json
{
  "name": "Armed Motion Alert",
  "enabled": true,
  "cooldown": 60,
  "triggers": [
    {
      "type": "vapix_event",
      "topic0": { "tns1": "RuleEngine" },
      "topic1": { "tnsaxis": "MotionDetection" },
      "filter_key": "active",
      "filter_value": true
    }
  ],
  "conditions": [
    { "type": "variable_compare", "name": "system.armed", "op": "eq", "value": "true" }
  ],
  "actions": [
    { "type": "recording", "operation": "start", "duration": 60 },
    {
      "type": "telegram",
      "bot_token": "YOUR_BOT_TOKEN",
      "chat_id": "YOUR_CHAT_ID",
      "message": "🚨 Motion at {{time}} on {{camera.model}}",
      "attach_snapshot": true
    }
  ]
}
```

### Example 4: Sunset → Night Mode + IR Light

```json
{
  "name": "Sunset → Night Mode",
  "enabled": true,
  "max_executions": 1,
  "max_exec_period": "day",
  "triggers": [
    {
      "type": "schedule",
      "schedule_type": "astronomical",
      "event": "sunset",
      "offset_minutes": 0
    }
  ],
  "conditions": [],
  "actions": [
    { "type": "ir_cut_filter", "mode": "night", "channel": 1 },
    { "type": "light_control", "operation": "on", "light_id": "led0", "intensity": 80 }
  ]
}
```

### Example 5: Webhook → Cross-Camera Action Chain

```json
{
  "name": "Webhook → Record + Alert on Remote Camera",
  "enabled": true,
  "triggers": [
    { "type": "http_webhook", "token": "perimeter-breach" }
  ],
  "conditions": [],
  "actions": [
    { "type": "recording", "operation": "start", "duration": 120 },
    {
      "type": "audio_clip",
      "clip_name": "warning.wav",
      "remote_host": "192.168.1.50",
      "remote_user": "root",
      "remote_pass": "pass"
    },
    {
      "type": "io_output",
      "port": 1,
      "state": "active",
      "duration": 30,
      "remote_host": "192.168.1.60",
      "remote_user": "root",
      "remote_pass": "pass"
    }
  ]
}
```

### Example 6: Daily Counter Reset

```json
{
  "name": "Reset Motion Counter at Midnight",
  "enabled": true,
  "triggers": [
    {
      "type": "schedule",
      "schedule_type": "daily_time",
      "time": "00:00",
      "days": [0, 1, 2, 3, 4, 5, 6]
    }
  ],
  "conditions": [],
  "actions": [
    {
      "type": "set_variable",
      "name": "motion_count",
      "value": "0"
    }
  ]
}
```

### Example 7: Air Quality → Speaker Display + Alert Escalation

```json
{
  "name": "CO2 Alert → Display Warning",
  "enabled": true,
  "cooldown": 300,
  "triggers": [
    {
      "type": "vapix_event",
      "topic0": { "tns1": "Device" },
      "topic1": { "tnsaxis": "AirQuality" },
      "value_key": "CO2",
      "value_op": "gt",
      "value_threshold": 1000
    }
  ],
  "conditions": [],
  "actions": [
    {
      "type": "speaker_display",
      "operation": "show",
      "message": "⚠ HIGH CO₂: {{trigger.CO2}} ppm — Please ventilate!",
      "textColor": "#FFFFFF",
      "backgroundColor": "#CC0000",
      "textSize": "large",
      "scrollDirection": "fromRightToLeft",
      "scrollSpeed": 5,
      "duration_type": "time",
      "duration_value": 60000
    },
    { "type": "set_variable", "name": "system.airquality_alert", "value": "active" }
  ]
}
```

---

## Common Patterns

### Pattern: State Machine with Variables
Use `set_variable` and `variable_compare` to build state machines. One rule sets the state, another checks it.

### Pattern: Rule Chaining
Rule A fires → `run_rule` triggers Rule B → `rule_fired` trigger on Rule C chains further.

### Pattern: Enable/Disable Rules Dynamically
Use `set_rule_enabled` in one rule to activate or deactivate other rules at runtime, creating mode-switching behavior (e.g. armed/disarmed, maintenance mode).

### Pattern: Cross-Device Orchestration
- Use `http_request` to POST to another camera's `/local/acap_event_engine/fire` endpoint with a webhook token
- Use `remote_host`/`remote_user`/`remote_pass` on actions to directly control remote Axis devices
- Use `vapix_event_state` or `io_state` conditions with `remote_host` to check state on other cameras

### Pattern: Sensor Data Pipeline
1. Schedule trigger (interval) → 2. `vapix_query` action fetches sensor data → 3. `influxdb_write`/`mqtt_publish`/`http_request` sends it upstream

### Pattern: Alert Throttling
Use `cooldown` to prevent floods. Use `max_executions` + `max_exec_period` to cap alerts (e.g. max 10 per hour).

### Pattern: While-Active Behavior
Set `while_active: true` on recording, overlay, I/O output, siren, or speaker_display actions to automatically stop/revert when the triggering event goes inactive.

---

## Important Notes & Constraints

1. **Authentication**: All API calls require HTTP Digest auth with admin credentials.
2. **HTTPS with self-signed certs**: Use `-k` (curl) or disable certificate verification in code.
3. **Rule IDs**: Auto-assigned UUIDs on creation. To update, use `POST /rules?id=UUID`.
4. **MQTT must be configured** before `mqtt_message` triggers or `mqtt_publish` actions will work.
5. **SMTP must be configured** before `email` actions will work.
6. **Location must be set** for `astronomical` schedule triggers and `day_night` conditions (or specify `latitude`/`longitude` per trigger/condition).
7. **Template variables** are expanded at execution time, not at rule creation time.
8. **`vapix_query` must come before** actions that use its injected `{{trigger.FIELD}}` values.
9. **Counter and variable names** are global and shared across all rules.
10. **`delay` actions** are asynchronous — they don't block other rules from executing.
11. **Bulk import** requires a JSON array, not a single object. Use `?action=import`.
12. **`while_active`** only works with stateful triggers (events that have active/inactive states).
13. **Maximum rule name length**: 100 characters.
14. **Maximum webhook token length**: 120 characters.
15. **Speaker display message maximum**: 1000 characters.
16. **Modbus TCP port 502**: Works for outbound client connections on all AXIS OS versions (including 12+). The privilege restriction only applies to binding/listening, not connecting.
17. **Modbus `op` field**: Use word-form operators — `gt`, `gte`, `lt`, `lte`, `eq`, `between`. Symbolic forms like `>=` are not accepted.

---

## Trigger: `modbus_read`

Polls a Modbus register at a configurable interval and fires when the value satisfies a threshold condition. Uses edge-triggered hysteresis — fires once when condition becomes true, does not re-fire until condition clears and becomes true again.

```json
{
  "type": "modbus_read",
  "connection_type": "tcp",
  "host": "192.168.1.50",
  "port": 502,
  "slave_id": 1,
  "register_type": "holding",
  "register": 0,
  "poll_interval": 10,
  "op": "gte",
  "threshold": 100
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `connection_type` | `"tcp"` \| `"rtu"` | `"tcp"` | Transport type |
| `host` | string | `"127.0.0.1"` | TCP: hostname or IP of Modbus server |
| `port` | integer | `502` | TCP: port number |
| `device` | string | `"/dev/ttyS1"` | RTU: serial device path |
| `baud` | integer | `9600` | RTU: baud rate |
| `parity` | `"N"` \| `"E"` \| `"O"` | `"N"` | RTU: parity |
| `slave_id` | integer | `1` | Modbus slave/unit ID |
| `register_type` | `"holding"` \| `"input"` \| `"coil"` \| `"discrete"` | `"holding"` | Register type (FC03/FC04/FC01/FC02) |
| `register` | integer | `0` | Register address (0-based) |
| `poll_interval` | integer | `30` | Seconds between polls (minimum 1) |
| `op` | `"gt"` \| `"gte"` \| `"lt"` \| `"lte"` \| `"eq"` \| `"between"` | — | Threshold comparison operator |
| `threshold` | number | — | Lower threshold value |
| `threshold2` | number | — | Upper threshold (required for `between`) |

Template variables injected: `{{trigger.value}}`, `{{trigger.register}}`, `{{trigger.register_type}}`, `{{trigger.slave_id}}`

---

## Action: `modbus_write`

Writes a value to a Modbus register. Uses FC06 for holding registers, FC05 for coils.

```json
{
  "type": "modbus_write",
  "connection_type": "tcp",
  "host": "192.168.1.50",
  "port": 502,
  "slave_id": 1,
  "register_type": "holding",
  "register": 10,
  "value": 1
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `connection_type` | `"tcp"` \| `"rtu"` | `"tcp"` | Transport type |
| `host` | string | `"127.0.0.1"` | TCP: hostname or IP |
| `port` | integer | `502` | TCP: port number |
| `device` | string | `"/dev/ttyS1"` | RTU: serial device path |
| `baud` | integer | `9600` | RTU: baud rate |
| `parity` | `"N"` \| `"E"` \| `"O"` | `"N"` | RTU: parity |
| `slave_id` | integer | `1` | Modbus slave/unit ID |
| `register_type` | `"holding"` \| `"coil"` | `"holding"` | Register type |
| `register` | integer | `0` | Register address (0-based) |
| `value` | number | — | Value to write. For coils: 0 or 1. For holding: 0–65535 |
