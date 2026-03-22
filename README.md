# Event Engine - ACAP for Axis Cameras

A powerful IFTTT(If This Then That)-style automation engine that runs directly on your Axis camera. Build rules that react to camera events, schedules, MQTT messages, or webhooks and respond with HTTP requests, MQTT publishes, recordings, PTZ moves, overlays, I/O outputs, siren/light signals, and more.

---

## What It Does

Event Engine replaces and extends the built-in Axis event system with a flexible **If This Then That** rule engine. Each rule has:

- **Triggers** - what starts the rule (one or more, evaluated as OR or AND)
- **Conditions** - optional checks that must pass before actions run
- **Actions** - what happens when the rule fires (one or more, run in sequence)

Rules are built in a clean web UI and take effect immediately - no reboot required.

<img width="397" height="591" alt="Screenshot 2026-03-21 at 06 14 23" src="https://github.com/user-attachments/assets/eacb1931-904c-409a-a9c2-2649cf5255d1" />
<img width="406" height="591" alt="Screenshot 2026-03-21 at 06 14 58" src="https://github.com/user-attachments/assets/ce711052-7bc4-41ad-9599-721cefb50ab2" />

---

## Triggers

| Type | Description |
|------|-------------|
| **VAPIX Event** | Any camera event (motion, thermometry, tampering, I/O, analytics, air quality, etc.) selected from a live dropdown. Supports an optional value condition - boolean match, or numeric threshold (is above / is below / equals / is between) with an optional hold duration requiring the condition to persist for N seconds before firing |
| **Schedule** | Cron expression, fixed interval, daily time with day-of-week selection, or **Sunrise/Sunset** (astronomical events: sunrise, sunset, civil dawn, civil dusk with optional offset in minutes and configurable latitude/longitude) |
| **MQTT Message** | Incoming MQTT message on a topic (wildcards supported, optional payload filter) |
| **HTTP Webhook** | External POST request with a secret token |
| **I/O Input** | Digital input port state change (rising/falling/both edges) with optional hold duration |
| **Counter Threshold** | When a named counter crosses a configured value |
| **Rule Chain** | Fires when another named rule executes |

## Conditions

| Type | Description |
|------|-------------|
| **Time Window** | Only allow firing between two times of day |
| **I/O State** | Check the current state of an I/O port |
| **Counter Compare** | Compare a counter value against a threshold |
| **Variable Compare** | Compare a named variable against a value |
| **HTTP Check** | Make an HTTP request; pass only if the response matches an expected status, body substring, or **JSONPath value** (dot-notation path into a JSON response, e.g. `data.temperature`) |

## Actions

| Type | Description |
|------|-------------|
| **HTTP Request** | GET, POST, PUT, or DELETE to any URL. Optional **snapshot attachment** (fetches a JPEG and makes it available as `{{trigger.snapshot_base64}}`). Optional **fallback action** executed when the request fails (non-2xx or network error) - log, MQTT publish, or secondary HTTP request |
| **MQTT Publish** | Publish a message to a topic with configurable QoS and retain flag |
| **Recording** | Start or stop a recording |
| **Overlay Text** | Write text to the video stream with an optional auto-remove duration |
| **PTZ Preset** | Move the camera to a named preset position |
| **I/O Output** | Set a digital output port high or low |
| **Audio Clip** | Play a named media clip on the camera |
| **Siren / Light** | Start or stop a named siren/LED profile on devices that support it (e.g. Axis D6310) |
| **VAPIX Event Query** | Fetch the latest cached data from a VAPIX event and inject it as `{{trigger.FIELD}}` variables for subsequent actions - useful for polling sensor values on a schedule trigger |
| **Set Variable** | Create or update a named persistent variable |
| **Increment Counter** | Add or subtract a value from a named counter |
| **Fire Rule** | Immediately trigger another rule by name |
| **Delay** | Pause the action sequence for N seconds before continuing |
| **Fire VAPIX Event** | Fire a custom VAPIX event visible to other Axis applications |
| **Syslog** | Write a message to the system log |

---

## Rule Settings

Each rule has two optional execution controls:

- **Cooldown** - minimum seconds between firings. Prevents alert floods when a trigger fires repeatedly.
- **Max Executions** - limit how many times the rule can fire, with a configurable period: per minute, per hour, per day, or lifetime total. The period counter resets automatically; the lifetime counter resets when the rule is saved.

### Arm / Disarm Pattern

Use the **Variable Compare** condition with a variable named `system.armed` (value `"true"` or `"false"`) to make rules only fire when the system is armed. Example rules shipped in the default template:

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

The built-in MQTT client supports MQTT 3.1.1 over raw POSIX sockets (no external library). Features:

- QoS 0 and QoS 1 publish
- Wildcard topic subscriptions (`+` and `#`)
- Automatic reconnect with exponential backoff
- TLS support
- Password stored separately from the API response and never exposed via GET

Configure broker, port, credentials, client ID, and a base topic in the **MQTT** tab.

---

## Web UI

Accessible at `http://<camera-ip>/local/acap_event_engine/index.html`

- **Rules** - create, edit, duplicate, enable/disable, and delete rules
- **Event Log** - per-rule firing history with timestamps and result codes
- **Variables** - view and manage named variables and counters
- **Settings** - engine settings (location for sunrise/sunset), MQTT broker configuration, device info, and backup/restore

---

## Requirements

- Axis camera running **AXIS OS 11.0 or later**
- [Docker](https://www.docker.com/) - only needed if building from source

---

## Install

Download the latest `.eap` from [Releases](../../releases) and install via the camera's web interface:

1. Go to `http://<camera-ip>/#settings/apps`
2. Click **Add app** and upload the `.eap` for your camera's architecture:
   - `Event_Engine_1_0_0_aarch64.eap` - Cortex-A53 and newer (most cameras from ~2017 onwards)
   - `Event_Engine_1_0_0_armv7hf.eap` - Cortex-A9 (older cameras)
3. Start the app

If you're unsure which architecture your camera uses, check **System → About** in the camera web interface, or look up the model in the [Axis Product Selector](https://www.axis.com/en-gb/support/product-selector).

---

## Build From Source

```bash
./build.sh
```

Requires Docker. The build pulls `axisecp/acap-native-sdk:12.0.0` automatically and produces both `.eap` files.

---

## API

All endpoints are under `/local/acap_event_engine/` and require HTTP Basic Auth (admin credentials).

| Method | Path | Description |
|--------|------|-------------|
| GET / POST / DELETE | `/rules` | Rule CRUD - POST without `id` creates, POST with `?id=` updates |
| POST | `/fire` | Fire a rule manually or via webhook token |
| GET | `/triggers` | Available trigger types and their schemas |
| GET | `/actions` | Available action types and their schemas |
| GET / POST / DELETE | `/variables` | Named variables and counters |
| GET | `/events` | Event log (most recent rule firings) |
| GET | `/engine` | Engine status, MQTT status, device info |
| GET / POST | `/settings` | Engine and MQTT configuration |

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

Rules and settings are stored in `localdata/` on the camera and survive in-place application updates. To preserve data across a full uninstall/reinstall, use the **Export** buttons in the Status tab to download your rules and settings as JSON files before uninstalling.

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
│   ├── app.js              # UI logic
│   ├── style.css           # Dark theme
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
