# Event Engine - ACAP for Axis Cameras

<img src="event_engine_icon_dark.svg" alt="Event Engine" width="72" align="right">

A powerful IFTTT (If This Then That)-style automation engine that runs directly on your Axis camera. Build rules that react to camera events, schedules, MQTT messages, or webhooks and respond with HTTP requests, MQTT publishes, recordings, PTZ moves, overlays, I/O outputs, siren/light signals, and more.

Want to know why you need this ACAP, take a look at [Use Cases](../../blob/main/use-cases/USE_CASES.md)

To get started right away, download the latest `.eap` from [Releases](../../releases) and install via the camera's web interface.

A full user manual is built in to the ACAP and available [Here](https://mo3he.github.io/Event-Engine-ACAP/help.html)

---

## What It Does

Event Engine replaces and extends the built-in Axis event system with a flexible **If This Then That** rule engine. Each rule has:

- **Triggers** - what starts the rule (one or more, evaluated as OR or AND)
- **Conditions** - optional checks that must pass before actions run
- **Actions** - what happens when the rule fires (one or more, run in sequence)

Rules are built in a clean web UI and take effect immediately.

<img width="397" height="591" alt="Screenshot 2026-03-21 at 06 14 23" src="https://github.com/user-attachments/assets/eacb1931-904c-409a-a9c2-2649cf5255d1" />
<img width="406" height="591" alt="Screenshot 2026-03-21 at 06 14 58" src="https://github.com/user-attachments/assets/ce711052-7bc4-41ad-9599-721cefb50ab2" />

---

## Triggers

| Type | Description |
|------|-------------|
| **Device Event** | Any device event (motion, thermometry, tampering, I/O, analytics, air quality, etc.) selected from a live dropdown. Supports an optional value condition - boolean match, or numeric threshold (is above / is below / equals / is between) with an optional hold duration requiring the condition to persist for N seconds before firing |
| **Schedule** | Cron expression, fixed interval, daily time with day-of-week selection, or **Sunrise/Sunset** (astronomical events: sunrise, sunset, civil dawn, civil dusk with optional offset in minutes and configurable latitude/longitude) |
| **MQTT Message** | Incoming MQTT message on a topic (wildcards supported, optional payload filter) |
| **HTTP Webhook** | External POST request with a secret token (max 120 characters). Requires admin-level camera credentials |
| **I/O Input** | Digital input port state change (rising/falling/both edges) with optional hold duration |
| **Counter Threshold** | When a named counter crosses a configured value |
| **Rule Chain** | Fires when another named rule executes |
| **AOA Scenario** | Fires when an Axis Object Analytics scenario generates an event. Optional object-class filter (human, car, truck, bus, bike, other vehicle, or any). Injects `{{trigger.scenario_id}}` and `{{trigger.object_class}}` |

## Conditions

| Type | Description |
|------|-------------|
| **Time Window** | Only allow firing between two times of day |
| **I/O State** | Check the current state of an I/O port |
| **Counter Compare** | Compare a counter value against a threshold |
| **Variable Compare** | Compare a named variable against a value |
| **HTTP Check** | Make an HTTP request; pass only if the response matches an expected status, body substring, or **JSONPath value** (dot-notation path into a JSON response, e.g. `data.temperature`) |
| **AOA Occupancy** | Poll Axis Object Analytics occupancy for a scenario and pass only if the count satisfies a threshold (gt / gte / lt / lte / eq). Filters by object class or uses the total count |
| **Day / Night** | Pass only during daytime (after sunrise) or nighttime (after sunset). Uses the sunrise/sunset engine with latitude/longitude from Location. The UI shows today's computed sunrise and sunset times. Optional per-condition lat/lon override |
| **Device Event State** | Check the current state of any device event by polling event instances. Match a topic substring and verify that a data key equals an expected value (e.g. is motion currently active?) |

## Actions

Actions are grouped by category in the rule editor.

### Notifications

| Type | Description |
|------|-------------|
| **HTTP Request** | GET, POST, PUT, or DELETE to any URL. Optional **snapshot attachment** (fetches a JPEG and makes it available as `{{trigger.snapshot_base64}}`). Optional **fallback action** executed when the request fails (non-2xx or network error) - log, MQTT publish, or secondary HTTP request |
| **MQTT Publish** | Publish a message to a topic with configurable QoS and retain flag |
| **Slack** | Send a message to a Slack channel via incoming webhook. Optional channel and username override |
| **Teams** | Send an Adaptive Card to Microsoft Teams via Power Automate / Workflows webhook. Optional title and theme colour |
| **Telegram** | Send a message via Telegram Bot API with Markdown or HTML formatting and link preview toggle |
| **Email (SMTP)** | Send an email with template-aware subject and body. SMTP server, credentials, and from address are configured once in the Settings tab — each action only needs the recipient, subject, and body |
| **Snapshot Upload** | Capture a JPEG from the camera and POST or PUT it to a URL with optional Basic Auth |
| **FTP Upload** | Capture a JPEG and upload it to an FTP or SFTP server. Template-aware path (e.g. `ftp://server/{{camera.serial}}/{{date}}_{{time}}.jpg`). Directories are created automatically |
| **Send Syslog** | Write a message to the system log |

### Camera

| Type | Description |
|------|-------------|
| **Recording** | Start or stop a recording |
| **Overlay Text** | Write text to the video stream with an optional auto-remove duration |
| **PTZ Preset** | Move the camera to a named preset position |
| **Guard Tour** | Start or stop a configured PTZ guard tour. Available tours are loaded from the camera and shown in a dropdown |
| **IR Cut Filter** | Force the IR cut filter **On** (day mode), **Off** (night mode), or restore **Auto** switching |
| **Privacy Mask** | Enable or disable a named privacy mask |
| **Wiper** | Trigger the windshield wiper |
| **Light Control** | Control an Axis illuminator (white light or IR LED) with optional intensity |

### Audio / Visual

| Type | Description |
|------|-------------|
| **Audio Clip** | Play a named media clip on the camera |
| **Siren / Light** | Start or stop a named siren/LED profile on devices that support it (e.g. Axis D6310) |

### I/O

| Type | Description |
|------|-------------|
| **I/O Output** | Set a digital output port high or low with an optional duration |

### Logic

| Type | Description |
|------|-------------|
| **Notification Digest** | Buffer events and send a batched summary at a configurable interval (minimum 30 seconds). Delivers via Slack, Teams, Telegram, Email, or MQTT. Each event adds one line using a template; all lines are combined into a single message on flush |
| **Delay** | Pause the action sequence for N seconds before continuing |
| **Set Variable** | Create or update a named persistent variable |
| **Increment Counter** | Add or subtract a value from a named counter |
| **Run Rule** | Immediately trigger another rule by name |

### Advanced

| Type | Description |
|------|-------------|
| **Fire ACAP Event** | Fire an ACAP event visible to other Axis applications |
| **Device Event Query** | Fetch the latest cached data from a device event and inject it as `{{trigger.FIELD}}` variables for subsequent actions - useful for polling sensor values on a schedule trigger |
| **Set Device Parameter** | Update any camera parameter via `param.cgi`. Tab out of the parameter field to look up the current value, allowed values, type, and range directly from the camera. **Expert users only — incorrect values can disrupt camera operation** |
| **ACAP Control** | Start, stop, or restart another installed ACAP application |

### Data

| Type | Description |
|------|-------------|
| **InfluxDB Write** | Write a data point to InfluxDB v1 or v2 using line protocol. Template-aware measurement, tags, and fields. v1 authenticates with username/password, v2 with API token |

### Analytics

| Type | Description |
|------|-------------|
| **AOA Get Counts** | Fetch accumulated crossline counts from an Axis Object Analytics scenario and inject them as template variables: `{{aoa_total}}`, `{{aoa_human}}`, `{{aoa_car}}`, `{{aoa_truck}}`, `{{aoa_bus}}`, `{{aoa_bike}}`, `{{aoa_otherVehicle}}`, `{{aoa_timestamp}}`. Optional **Reset after fetch** to zero the counters after reading |

---

## Rule Settings

Each rule has two optional execution controls:

- **Cooldown** - minimum seconds between firings. Prevents alert floods when a trigger fires repeatedly.
- **Max Executions** - limit how many times the rule can fire, with a configurable period: per minute, per hour, per day, or lifetime total. The period counter resets automatically; the lifetime counter resets when the rule is saved.

### Arm / Disarm Pattern

Use the **Variable Compare** condition with a variable named `system.armed` (value `"true"` or `"false"`) to make rules only fire when the system is armed. Example rules shipped in the templates:

1. **Arm System via MQTT** - subscribe to `cameras/<serial>/arm`, set `system.armed = "true"`
2. **Disarm System via MQTT** - same topic, set `system.armed = "false"`
3. **Motion Alert When Armed** - motion trigger with a Variable Compare condition on `system.armed = "true"`

---

## Dynamic Variables

Action fields (URL, body, MQTT payload, overlay text, syslog message, etc.) support `{{variable}}` substitution. Use the **Insert variable** button in the rule editor to pick from available values.

| Variable | Value |
|----------|-------|
| `{{timestamp}}` | ISO 8601 UTC timestamp |
| `{{date}}` | YYYY-MM-DD |
| `{{time}}` | HH:MM:SS |
| `{{camera.serial}}` | Camera serial number |
| `{{camera.model}}` | Camera model name |
| `{{camera.ip}}` | Camera IP address |
| `{{trigger_json}}` | Full trigger event data as a compact JSON string |
| `{{trigger.KEY}}` | Individual field from the trigger event (e.g. `{{trigger.CO2}}`) |
| `{{var.NAME}}` | Value of a named variable |
| `{{counter.NAME}}` | Value of a named counter |
| `{{trigger.snapshot_base64}}` | Base64-encoded JPEG snapshot (only when **Attach snapshot** is enabled on the HTTP Request action) |

The rule editor shows which `{{trigger.*}}` keys are available for the selected trigger type.

**Example - MQTT payload with sensor data:**
```
Camera {{camera.serial}} at {{timestamp}}: {{trigger_json}}
```

---

## MQTT

The built-in MQTT client supports MQTT 3.1.1 with the following features:

- QoS 0 and QoS 1 publish
- Wildcard topic subscriptions (`+` and `#`)
- Automatic reconnect with exponential backoff
- **TLS/SSL encryption** for secure broker connections
- Password stored separately from the API response and never exposed via GET

Configure broker, port, credentials, client ID, and a base topic in the **MQTT** tab.

---

## Web UI

Accessible at `http://<camera-ip>/local/acap_event_engine/index.html`

- **Rules** - create, edit, duplicate, enable/disable, and delete rules
- **Event Log** - per-rule firing history with timestamps and result codes
- **Variables** - view and manage named variables and counters
- **Settings** - Location (used for sunrise/sunset), SMTP configuration, MQTT broker configuration, device info, and backup/restore

---

## Requirements

- Axis camera running **AXIS OS 11.0 or later**
- [Docker](https://www.docker.com/) - only needed if building from source

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

Requires Docker. The build pulls `axisecp/acap-native-sdk:12.0.0` automatically and produces `.eap` files.

---

## API

All endpoints are under `/local/acap_event_engine/` and require HTTP Basic Auth (admin credentials).

| Method | Path | Description |
|--------|------|-------------|
| GET / POST / DELETE | `/rules` | Rule CRUD - POST without `id` creates, POST with `?id=` updates, GET with `?id=<id>&action=export` downloads rule as JSON |
| POST | `/fire` | Fire a rule manually or via webhook token |
| GET | `/triggers` | Available trigger types and their schemas |
| GET | `/actions` | Available action types and their schemas |
| GET / POST / DELETE | `/variables` | Named variables and counters |
| GET | `/events` | Event log (most recent rule firings) |
| GET | `/engine` | Engine status, MQTT status, device info |
| GET | `/aoa` | List configured Axis Object Analytics scenarios (id, name, type) |
| GET / POST | `/settings` | Engine, MQTT, SMTP, and device configuration |

Full spec: `app/html/openapi.json`
Interactive docs: `http://<camera-ip>/local/acap_event_engine/swagger.html`

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
├── manifest.json           # ACAP package manifest (schemaVersion 1.4.0)
├── Makefile
├── engine/
│   ├── rule_engine.c       # Rule store, trigger dispatch, cooldown, rate limiting
│   ├── triggers.c          # All trigger types - subscribe, match, threshold, hold duration
│   ├── conditions.c        # Condition evaluation
│   ├── actions.c           # All action types + {{variable}} template engine
│   ├── scheduler.c         # Cron, interval, and daily-time scheduler
│   ├── mqtt_client.c       # MQTT 3.1.1 client over raw POSIX sockets
│   ├── variables.c         # Named variables and counters (persistent)
│   └── event_log.c         # In-memory ring-buffer event log
├── html/
│   ├── index.html          # Web UI shell
│   ├── api.js              # API client layer
│   ├── app.js              # Main UI logic and event dispatcher
│   ├── device-capabilities.js # Camera capability detection (PTZ, audio, AOA, etc.)
│   ├── settings.js         # Settings tab and device integration
│   ├── rule-editor.js      # Full rule editor with 2200+ lines
│   ├── style.css           # Light and dark theme
│   ├── swagger.html        # Swagger UI
│   └── openapi.json        # OpenAPI 3.0 spec
└── settings/
    ├── settings.json       # Default settings
    ├── events.json         # Declared VAPIX events (RuleFired)
    └── default_rules.json  # Example rules loaded on first run
```

---

## License

MIT
