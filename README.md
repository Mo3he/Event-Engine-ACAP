<img src="event_engine_icon_dark.svg" alt="Event Engine" width="72" align="right">

# Event Engine - ACAP for Axis Cameras

### Check out the [Homepage](https://mo3he.github.io/Event-Engine-ACAP/) for a full overview of everything Event Engine can do
A powerful If-This-Then-That style automation engine that runs directly on your Axis camera. Build rules that react to camera events, schedules, MQTT messages, or webhooks and respond with HTTP requests, MQTT publishes, recordings, PTZ moves, overlays, I/O outputs, siren/light signals, and more.

#### To get started right away, download the latest `.eap` from [Releases](../../releases) and install via the camera's web interface.

Want to know why you need this ACAP? Take a look at [Use Cases](https://mo3he.github.io/Event-Engine-ACAP/use-cases.html)  
A full user manual is built in to the ACAP and available here: [User Manual](https://mo3he.github.io/Event-Engine-ACAP/help.html)  
API documentation can be found here: [API Docs](https://petstore.swagger.io/?url=https://raw.githubusercontent.com/Mo3he/Event-Engine-ACAP/main/app/html/openapi.json)

If you find something that doesn't work the way it should, open an [Issue](https://github.com/Mo3he/Event-Engine-ACAP/issues), if you would like me to add something, start a [Discussion](https://github.com/Mo3he/Event-Engine-ACAP/discussions) and if you like my work, a coffee would help a lot

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-orange?style=flat&logo=buy-me-a-coffee)](https://www.buymeacoffee.com/mo3he)

> **Disclaimer:** This is an independent, community-developed ACAP package and is not an official Axis Communications product. It is not affiliated with, endorsed by, or supported by Axis Communications AB. Use it at your own risk. For official Axis software, visit axis.com 

---

## What It Does

Event Engine replaces and extends the built-in Axis event system with a flexible **If This Then That** rule engine.

Each rule has:

- **Triggers** — what starts the rule (one or more)
- **Conditions** — optional checks that must pass before actions run (one or more)
- **Actions** — what happens when the rule fires (one or more, run in sequence)

**Trigger logic** can be set to `OR` (any trigger fires the rule), `AND` (all triggers must fire within a configurable time window), or `AND_ACTIVE` (all triggers must be simultaneously in their active state). Conditions can be combined as `AND` (all must pass) or `OR` (any passes).

Rules are built in a clean web UI.

<img width="410" height="591" alt="Screenshot 2026-03-21 at 06 14 58" src="https://github.com/user-attachments/assets/a01a9021-0cda-4d38-bd5a-27a52ec13a61" />

---

## Requirements

- Axis camera running **AXIS OS 11.11 or later** (required by the SDK used to build the package)
- [Docker](https://www.docker.com/) — only needed if building from source

---

## Install

Download the latest `.eap` from [Releases](../../releases) and install via the camera's web interface:

1. Go to `http://<camera-ip>/#settings/apps`
2. Click **Add app** and upload the `.eap` for your camera's architecture:

   - `aarch64` — newer Axis cameras (ARTPEC-8, most cameras from 2020+)
   - `armv7hf` — older Axis cameras (ARTPEC-6/7)
3. Start the app

If you're unsure which architecture your camera uses, check **System → About** in the camera web interface, or look up the model in the [Axis Product Selector](https://www.axis.com/en-gb/support/product-selector).

---

## Build From Source

```bash
./build.sh            # builds both aarch64 and armv7hf
./build.sh aarch64    # build only aarch64
./build.sh armv7hf    # build only armv7hf
```

Requires Docker. The build pulls `axisecp/acap-native-sdk:1.15.1` automatically and produces `.eap` files.

---

## Web UI

Accessible at `http://<camera-ip>/local/acap_event_engine/index.html`

- **Rules** — create, edit, duplicate, enable/disable, delete, import/export, and tag rules
- **Event Log** — per-rule firing history with timestamps and result codes
- **Variables** — view and manage named variables and counters
- **Settings** — location (used for sunrise/sunset), SMTP configuration, MQTT broker configuration, SOCKS5 proxy, device info, and backup/restore

---

## Triggers

| Type | Description |
|------|-------------|
| **Device Event** | Any device event (motion, thermometry, tampering, I/O, analytics, air quality, etc.) selected from a live dropdown. Supports an optional value condition — boolean match, or numeric threshold (is above / is below / equals / is between) with an optional hold duration requiring the condition to persist for N seconds before firing |
| **Schedule** | Cron expression, fixed interval, daily time with day-of-week selection, or **Sunrise/Sunset** (astronomical events: sunrise, sunset, civil dawn, civil dusk with optional offset in minutes and configurable latitude/longitude) |
| **MQTT Message** | Incoming MQTT message on a topic (wildcards `+`/`#` supported, optional payload filter) |
| **HTTP Webhook** | External POST request with a secret token (max 120 characters). Requires admin-level camera credentials. All keys in the JSON `payload` object are flattened and injected as `{{trigger.KEY}}` variables |
| **I/O Input** | Digital input port state change (rising/falling/both edges) with optional hold duration. Supervised inputs also support **cut** (open-circuit/wire cut) and **short** (short-circuit) fault states |
| **Counter Threshold** | When a named counter crosses a configured value (polled every second) |
| **Rule Chain** | Fires when another named rule executes |
| **AOA Scenario** | Fires when an Axis Object Analytics scenario generates an event. Optional object-class filter (human, car, truck, bus, bike, other vehicle, or any). Injects `{{trigger.scenario_id}}`, `{{trigger.active}}`, and `{{trigger.reason}}` |
| **Manual** | Fired via the **Fire Now** button in the UI or `POST /fire` with a rule ID. Useful for testing and one-shot actions |
| **Modbus Read** | Poll a Modbus register on a configurable interval and fire when the value crosses a threshold. Supports Modbus TCP and RTU/RS-485. Register types: holding (FC03), input (FC04), coil (FC01), discrete (FC02). Threshold operators: `gt`, `gte`, `lt`, `lte`, `eq`, `between`. Port 502 works on all firmware versions |

## Conditions

| Type | Description |
|------|-------------|
| **Time Window** | Only allow firing between two times of day on specified days. Supports wraparound (e.g. 22:00–06:00) |
| **Day / Night** | Pass only during daytime (after sunrise) or nighttime (after sunset). Uses the sunrise/sunset engine with latitude/longitude from Settings. The UI shows today's computed sunrise and sunset times. Optional per-condition lat/lon override |
| **I/O State** | Check the current state of an I/O port. Supports **remote device** target |
| **Counter Compare** | Compare a counter value against a threshold |
| **Variable Compare** | Compare a named variable against a value |
| **HTTP Check** | Make an HTTP request; pass only if the response matches an expected status, body substring, or **JSONPath value** (dot-notation path into a JSON response, e.g. `data.temperature`) |
| **AOA Occupancy** | Poll Axis Object Analytics occupancy for a scenario and pass only if the count satisfies a threshold (gt / gte / lt / lte / eq). Filters by object class or uses the total count. Supports **remote device** target; use Load to fetch available scenarios from the remote device |
| **Device Event State** | Check the current state of any device event by polling event instances. Match a topic substring and verify that a data key equals an expected value (e.g. is motion currently active?). Supports **remote device** target; use Load to fetch the full event catalog from the remote device |

---

## Actions

Actions are grouped by category in the rule editor. Actions that support **while_active** automatically undo themselves when the trigger deactivates (e.g. stop a siren when motion ends).

### Notifications

| Type | Description |
|------|-------------|
| **HTTP Request** | GET, POST, PUT, or DELETE to any URL. Optional **snapshot attachment** (fetches a JPEG and makes it available as `{{trigger.snapshot_base64}}`). Optional **fallback action** executed when the request fails (non-2xx or network error) — log, MQTT publish, or secondary HTTP request |
| **MQTT Publish** | Publish a message to a topic with configurable QoS (0 or 1) and retain flag |
| **Slack** | Send a message to a Slack channel via incoming webhook. Optional channel and username override |
| **Teams** | Send an Adaptive Card to Microsoft Teams via Power Automate / Workflows webhook. Optional title and theme colour |
| **Telegram** | Send a message via Telegram Bot API with Markdown or HTML formatting and link preview toggle |
| **Email (SMTP)** | Send an email with template-aware subject and body. Optional **snapshot attachment**. SMTP server, credentials, and from address are configured once in the Settings tab — each action only needs the recipient, subject, and body |
| **Snapshot Upload** | Capture a JPEG from the camera and POST or PUT it to a URL with optional Basic Auth |
| **FTP Upload** | Capture a JPEG and upload it to an FTP or SFTP server. Template-aware path (e.g. `ftp://server/{{camera.serial}}/{{date}}_{{time}}.jpg`). Directories are created automatically. Optional **fallback action** on upload failure |
| **Send Syslog** | Write a message to the system log |

### Camera

| Type | Description |
|------|-------------|
| **Recording** | Start or stop a recording. Supports `while_active` auto-stop |
| **Overlay Text** | Write text to the video stream with an optional auto-remove duration. Supports `while_active` auto-clear |
| **PTZ Preset** | Move the camera to a named preset position |
| **Guard Tour** | Start or stop a configured PTZ guard tour. Available tours are loaded from the camera and shown in a dropdown |
| **IR Cut Filter** | Force the IR cut filter **On** (day mode), **Off** (night mode), or restore **Auto** switching |
| **Privacy Mask** | Enable or disable a named privacy mask |
| **Wiper** | Trigger the windshield wiper |
| **Light Control** | Control an Axis illuminator (white light or IR LED) with optional intensity |

### Audio / Visual

| Type | Description |
|------|-------------|
| **Audio Clip** | Play a named media clip on the camera with configurable volume (0–100), loop count, and output channel. Set `while_active` to loop indefinitely and auto-stop when the trigger deactivates |
| **Paging Console Execute** | Execute a pre-configured action on an Axis Paging Console (e.g. Axis C6110) by action UUID. Supports **remote device** target |
| **Paging Console Button** | Update a button slot on a Paging Console page to a specified action UUID, or clear it. GET the current layout, patch the slot, PUT it back atomically. Supports **remote device** target |
| **Siren / Light** | Start or stop a named siren/LED profile on devices that support it (e.g. Axis D6310). Supports `while_active` auto-stop |
| **Speaker Display** | Show text or a scrolling message on an Axis speaker display (e.g. Axis C1710). Configurable foreground/background colour, text size, scroll speed, and duration (indefinite, time, or repetitions). Supports `{{variable}}` substitution and `while_active` auto-clear. Supports **remote device** target |

### I/O

| Type | Description |
|------|-------------|
| **I/O Output** | Set a digital output port high or low with an optional duration. Supports `while_active` auto-reset. Supports **remote device** target |

### Logic

| Type | Description |
|------|-------------|
| **Notification Digest** | Buffer events and send a batched summary at a configurable interval (minimum 30 seconds). Delivers via Slack, Teams, Telegram, Email, or MQTT. Each event adds one line using a template; all lines are combined into a single message on flush |
| **Delay** | Pause the action sequence for N seconds before continuing |
| **Set Variable** | Create or update a named persistent variable |
| **Increment Counter** | Add, subtract, reset, or set a named counter |
| **Run Rule** | Immediately trigger another rule by name |
| **Enable / Disable Rule** | Enable or disable another rule by ID — use to activate seasonal rules, switch operational profiles, or suppress alerts from a central control system |

### Advanced

| Type | Description |
|------|-------------|
| **Fire ACAP Event** | Fire an ACAP event visible to the built-in event system and other Axis applications. Useful as a VMS bridge |
| **Device Event Query** | Fetch the latest data from a device event and inject it as `{{trigger.FIELD}}` variables for subsequent actions. Supports querying a **remote Axis device** (via IP/credentials) or the local passive subscription cache |
| **Set Device Parameter** | Update any camera parameter via `param.cgi`. Tab out of the parameter field to look up the current value, allowed values, type, and range directly from the camera. **Expert users only — incorrect values can disrupt camera operation** |
| **ACAP Control** | Start, stop, or restart another installed ACAP application |

### Data

| Type | Description |
|------|-------------|
| **InfluxDB Write** | Write a data point to InfluxDB v1 or v2 using line protocol. Template-aware measurement, tags, and fields. v1 authenticates with username/password, v2 with API token |

### Modbus

| Type | Description |
|------|-------------|
| **Modbus Write** | Write a value to a Modbus register via TCP or RTU/RS-485. Supports FC06 (write single holding register) and FC05 (write single coil). Uses the same connection parameters as Modbus Read |

### Analytics

| Type | Description |
|------|-------------|
| **AOA Get Counts** | Fetch accumulated crossline counts from an Axis Object Analytics scenario and inject them as template variables: `{{aoa_total}}`, `{{aoa_human}}`, `{{aoa_car}}`, `{{aoa_truck}}`, `{{aoa_bus}}`, `{{aoa_bike}}`, `{{aoa_otherVehicle}}`, `{{aoa_timestamp}}`. Optional **Reset after fetch** to zero the counters after reading |

---

## Cross-Device Automation

Conditions and actions marked **remote device** can target any reachable Axis device on the network instead of the local camera. Supply an IP/hostname, username, and password in the condition or action block. This enables rules that detect on one device, verify state on a second, and act on a third — no server or middleware required.

**Conditions:** I/O State, AOA Occupancy (Load button fetches scenarios from remote), Device Event State (Load button fetches event catalog from remote)

**Actions:** Recording, Overlay Text, PTZ Preset, Guard Tour, IR Cut Filter, Privacy Mask, Wiper, Light Control, Audio Clip, Siren / Light, Speaker Display, I/O Output, Paging Console Execute, Paging Console Button, Device Event Query, Set Device Parameter, ACAP Control

---

## Rule Settings

Each rule has two optional execution controls:

- **Cooldown** — minimum seconds between firings. Prevents alert floods when a trigger fires repeatedly.
- **Max Executions** — limit how many times the rule can fire, with a configurable period: per minute, per hour, per day, or lifetime total. The period counter resets automatically; the lifetime counter resets when the rule is saved.

### Arm / Disarm Pattern

Use the **Variable Compare** condition with a variable named `system.armed` (value `"true"` or `"false"`) to make rules only fire when the system is armed. Example rules shipped in the templates:

1. **Arm System via MQTT** — subscribe to `cameras/<serial>/arm`, set `system.armed = "true"`
2. **Disarm System via MQTT** — same topic, set `system.armed = "false"`
3. **Motion Alert When Armed** — motion trigger with a Variable Compare condition on `system.armed = "true"`

---

## Dynamic Variables

Action fields (URL, body, MQTT payload, overlay text, syslog message, etc.) support `{{variable}}` substitution. Use the **Insert variable** button in the rule editor to pick from available values.

| Variable | Value |
|----------|-------|
| `{{timestamp}}` | ISO 8601 UTC timestamp |
| `{{date}}` | YYYY-MM-DD |
| `{{time}}` | HH:MM:SS |
| `{{camera.serial}}` | Camera serial number |
| `{{camera.name}}` | Camera model name |
| `{{camera.ip}}` | Camera IP address |
| `{{trigger_json}}` | Full trigger event data as a compact JSON string |
| `{{trigger.KEY}}` | Individual field from the trigger event (e.g. `{{trigger.CO2}}`) |
| `{{trigger.KEY\|N}}` | Numeric value rounded to N decimal places (e.g. `{{trigger.Temperature\|2}}` → `20.35`) |
| `{{var.NAME}}` | Value of a named variable |
| `{{counter.NAME}}` | Value of a named counter |
| `{{trigger.snapshot_base64}}` | Base64-encoded JPEG snapshot (only when **Attach snapshot** is enabled on the HTTP Request action) |

The rule editor shows which `{{trigger.*}}` keys are available for the selected trigger type.

**Example — MQTT payload with sensor data:**
```
Camera {{camera.serial}} at {{timestamp}}: {{trigger_json}}
```

---

## MQTT

The built-in MQTT client supports MQTT 3.1.1 with the following features:

- QoS 0 and QoS 1 publish and subscribe
- Wildcard topic subscriptions (`+` and `#`)
- Automatic reconnect with exponential backoff
- Keepalive with PINGREQ/PINGRESP
- **TLS/SSL encryption** for secure broker connections
- **SOCKS5 proxy** support for broker connections behind a proxy
- Thread-safe publish (mutex protected)
- Password stored separately from the API response and never exposed via GET

Configure broker, port, credentials, client ID, and TLS/proxy settings in the **MQTT** tab.

---

## Use Case Templates

The [`use-cases/`](use-cases/) directory contains 50 ready-to-import JSON rule templates across 7 categories:

1. **Notifications & Data** — sensor data to Power BI, InfluxDB, MQTT/Home Assistant; multi-platform motion alerts; daily activity digests
2. **Camera Control** — sunrise/sunset day/night mode switching; privacy mask scheduling with emergency override; PTZ track-and-resume; motion-triggered audio; scheduled wiper
3. **Security & Access** — perimeter intrusion response; business-hours access control with after-hours alerts; occupancy warnings
4. **Signage & Displays** — queue/ticket display; air quality monitoring on speaker displays; webhook-driven signage
5. **System Integration** — arm/disarm via MQTT; daily counter reset; MQTT heartbeat; maintenance mode; enable/disable rules via MQTT
6. **Cross-Device** — multi-device I/O with conditions, speaker display, and paging console orchestration
7. **Advanced Patterns** — counter-driven escalation; tailgate detection; night-only recording; snapshot FTP with fallback; ACAP watchdog; paging console announcements; VMS event bridge; security audit logger; network-gated alerts; raw snapshot upload; dual-zone AND_ACTIVE logic

Import a template: open the web UI → Rules tab → Import, or POST the JSON to `/local/acap_event_engine/rules?action=import`.

See [`use-cases/USE_CASES.md`](use-cases/USE_CASES.md) for full descriptions and setup instructions.

---

## API

All endpoints are under `/local/acap_event_engine/` and require Digest Auth (admin credentials).

| Method | Path | Description |
|--------|------|-------------|
| GET / POST / DELETE | `/rules` | Rule CRUD — POST without `id` creates, POST with `?id=` updates, `?action=export` exports all, `?action=import` bulk imports |
| POST | `/fire` | Fire a rule manually (`{"id":"UUID"}`) or via webhook token (`{"token":"...", "payload":{...}}`) |
| GET | `/triggers` | Available trigger types and their schemas |
| GET / POST | `/actions` | Available action types (GET) or test a single action (POST) |
| GET / POST / DELETE | `/variables` | Named variables and counters |
| GET / DELETE | `/events` | Event log (most recent rule firings). Optional `?limit=N&rule=UUID` filter |
| GET | `/engine` | Engine status, uptime, rule counts, events today, MQTT status, device info |
| GET | `/aoa` | List configured Axis Object Analytics scenarios (id, name, type) |
| GET / POST / DELETE | `/acapevents` | User-defined ACAP events (system events: RuleFired, RuleError, EngineReady) |
| GET / POST | `/settings` | Engine, MQTT, SMTP, and UI configuration |
| POST | `/remote-caps` | Proxy capability queries to remote Axis devices (ptz, audio, siren, privacy, guardtour, acap, param, aoa, vapix_events, paging) |
| GET | `/alertStream` | Real-time multipart/mixed event stream — see [Alert Stream](#alert-stream) |

Full spec: `app/html/openapi.json`
Interactive docs: `http://<camera-ip>/local/acap_event_engine/swagger.html`
AI automation guide: [`AI_RULE_GUIDE.md`](AI_RULE_GUIDE.md) — complete reference for programmatically creating and uploading rules via the API

### Alert Stream

A long-lived HTTP endpoint that pushes rule-fire events in real-time as `multipart/mixed` JSON chunks — no polling required. Digest Auth is handled by the camera's Apache proxy.

```bash
curl --digest -u admin:pass -N \
  "http://<camera-ip>/local/acap_event_engine/alertStream"
```

The first chunk arrives immediately on connect (`{"connected":true}`). Each subsequent chunk fires when a rule executes and includes:

| Field | Description |
|-------|-------------|
| `ipAddress` | Camera IP |
| `macAddress` | Camera serial number |
| `dateTime` | ISO 8601 UTC timestamp |
| `eventType` | Always `RuleFired` |
| `eventDescription` | Rule name |
| `rule_id` / `rule_name` | Rule identity |
| `trigger_data` | Raw trigger event fields (vary by trigger type) |

Up to 8 clients can be connected simultaneously. The stream URL is shown in the Settings tab of the web UI.

### Webhook Example

Trigger a rule from any external system:

```bash
curl -u admin:pass -X POST \
  "http://<camera-ip>/local/acap_event_engine/fire" \
  -H "Content-Type: application/json" \
  -d '{"token": "my-secret-token", "payload": {"source": "doorbell"}}'
```

---

## Persistence

Rules and settings are stored in `localdata/` on the camera and survive in-place application updates. To preserve data across a full uninstall/reinstall, use the **Export** buttons in the Settings tab to download your rules and settings as JSON files before uninstalling.

---

## Project Structure

```
app/
├── main.c                  # HTTP endpoints, initialisation, event dispatch
├── ACAP.c / ACAP.h         # Axis SDK wrapper (HTTP, events, files, device info, VAPIX)
├── cJSON.c / cJSON.h       # Bundled JSON library
├── manifest.json           # ACAP package manifest (schemaVersion 1.5.0)
├── Makefile
├── engine/
│   ├── rule_engine.c       # Rule store, trigger dispatch, cooldown, rate limiting
│   ├── triggers.c          # All trigger types — subscribe, match, threshold, hold duration
│   ├── conditions.c        # Condition evaluation (lightweight + heavy split)
│   ├── actions.c           # All action types + {{variable}} template engine
│   ├── scheduler.c         # Cron, interval, daily-time, and astronomical scheduler
│   ├── mqtt_client.c       # MQTT 3.1.1 client over POSIX sockets (TLS + SOCKS5)
│   ├── variables.c         # Named variables and counters (persistent)
│   ├── event_log.c         # In-memory ring-buffer event log (500 entries)
│   └── alert_stream.c      # Real-time multipart HTTP event stream (TCP on localhost:8888)
├── html/
│   ├── index.html          # Web UI shell
│   ├── api.js              # API client layer
│   ├── app.js              # Main UI logic and event dispatcher
│   ├── device-capabilities.js # Camera capability detection (PTZ, audio, AOA, etc.)
│   ├── settings.js         # Settings tab and device integration
│   ├── rule-editor.js      # Full rule editor
│   ├── style.css           # Light and dark theme
│   ├── swagger.html        # Swagger UI
│   └── openapi.json        # OpenAPI 3.0 spec
└── settings/
    ├── settings.json       # Default settings
    ├── events.json         # Declared VAPIX events (RuleFired, RuleError, EngineReady)
    └── default_rules.json  # Example rules loaded on first run
use-cases/
├── USE_CASES.md            # Detailed use-case descriptions
└── templates/              # 50 ready-to-import JSON rule templates
```

---

## License

MIT
