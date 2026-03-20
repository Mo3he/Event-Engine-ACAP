# ACAP Event Engine

An IFTTT-style rule engine for Axis cameras that replaces and extends the built-in event system. Build automation rules with multiple triggers, conditions, and actions — including MQTT, HTTP webhooks, scheduling, recording, PTZ, overlays, variables, counters, and more.

## Features

- **Rules** — multiple triggers (OR/AND logic), multiple conditions, multiple actions per rule
- **Triggers** — VAPIX event, HTTP webhook, MQTT message, schedule (cron/interval/daily), I/O input, counter threshold, rule chaining
- **Conditions** — time window, event state, counter comparison, HTTP check, I/O state, variable compare
- **Actions** — HTTP request, recording start/stop, overlay text, PTZ preset, I/O output, audio clip, syslog, VAPIX event, delay, set variable, increment counter, MQTT publish, run rule
- **MQTT** — full MQTT 3.1.1 client (raw sockets, no external library), wildcard topic subscriptions, reconnect with backoff
- **Templates** — `{{timestamp}}`, `{{camera.serial}}`, `{{trigger.KEY}}`, `{{var.NAME}}`, `{{counter.NAME}}` in action payloads
- **Scheduler** — cron expressions, fixed intervals, daily time with day-of-week selection
- **Variables & Counters** — named, persistent, usable in templates and conditions
- **Event Log** — per-rule history with result codes
- **Web UI** — dark-theme interface with rule editor, event log, variables, MQTT config, and status
- **API Docs** — OpenAPI 3.0 spec with Swagger UI at `/swagger.html`
- **Export/Import** — backup and restore rules and settings as JSON

## Requirements

- [Docker](https://www.docker.com/) — for cross-compilation
- [ACAP Native SDK 12.0.0](https://hub.docker.com/r/axisecp/acap-native-sdk) — pulled automatically by the build
- Axis camera running AXIS OS 11.x or later

## Build

```bash
./build.sh
```

Produces two installable packages:
- `Event_Engine_1_0_0_aarch64.eap` — for cameras with Cortex-A53+ (most cameras from ~2017+)
- `Event_Engine_1_0_0_armv7hf.eap` — for older cameras with Cortex-A9

## Install

1. Open your camera's web interface: `http://<camera-ip>/#settings/apps`
2. Click **Add app** and upload the `.eap` file matching your camera's architecture
3. Start the application

The UI is accessible at:
```
http://<camera-ip>/local/acap_event_engine/index.html
```

## API

All endpoints are under `/local/acap_event_engine/` and require HTTP Basic Auth with admin credentials.

| Method | Path | Description |
|--------|------|-------------|
| GET/POST/PUT/DELETE | `/rules` | Rule CRUD |
| POST | `/fire` | Fire a rule manually or via webhook token |
| GET | `/triggers` | Available trigger types |
| GET | `/actions` | Available action types |
| GET/POST/DELETE | `/variables` | Variables and counters |
| GET | `/events` | Event log |
| GET | `/engine` | Engine status, MQTT status, device info |
| GET/POST | `/settings` | Engine and MQTT configuration |

Full OpenAPI spec: `app/html/openapi.json`
Interactive docs: `http://<camera-ip>/local/acap_event_engine/swagger.html`

### Webhook

Trigger rules from any external system:

```bash
curl -u admin:pass -X POST \
  "http://<camera-ip>/local/acap_event_engine/fire" \
  -H "Content-Type: application/json" \
  -d '{"token": "my-secret-token", "payload": {"source": "doorbell"}}'
```

### Templates

Action fields (URL, body, message, MQTT payload, etc.) support `{{...}}` substitution:

```json
{
  "type": "http_request",
  "url": "https://example.com/notify",
  "body": "{\"camera\": \"{{camera.serial}}\", \"time\": \"{{timestamp}}\", \"zone\": \"{{trigger.source}}\"}"
}
```

| Token | Value |
|-------|-------|
| `{{timestamp}}` | ISO 8601 UTC timestamp |
| `{{date}}` | YYYY-MM-DD |
| `{{time}}` | HH:MM:SS |
| `{{camera.serial}}` | Camera serial number |
| `{{camera.model}}` | Camera model |
| `{{camera.ip}}` | Camera IP address |
| `{{trigger.KEY}}` | Any key from the trigger event data |
| `{{var.NAME}}` | Named variable value |
| `{{counter.NAME}}` | Named counter value |

## Persistence

Rules and settings are stored in the camera's `localdata/` directory and persist across application updates (in-place upgrades). To preserve data across a full uninstall/reinstall, use the **Export** buttons in the Status tab to download your rules and settings as JSON before uninstalling.

## Project Structure

```
app/
├── main.c                  # HTTP endpoints, init, event dispatch
├── ACAP.c / ACAP.h         # Axis SDK wrapper (HTTP, events, files, device info)
├── cJSON.c / cJSON.h       # Bundled JSON library
├── manifest.json           # ACAP package manifest
├── Makefile
├── engine/
│   ├── rule_engine.c       # Rule store, trigger dispatch, cooldown, chaining
│   ├── triggers.c          # VAPIX, webhook, schedule, MQTT, I/O, counter triggers
│   ├── conditions.c        # Time window, HTTP check, counter, variable conditions
│   ├── actions.c           # All action types + template engine
│   ├── scheduler.c         # Cron, interval, daily-time scheduler
│   ├── mqtt_client.c       # MQTT 3.1.1 over raw POSIX sockets
│   ├── variables.c         # Named variables and counters
│   └── event_log.c         # In-memory event log with ring buffer
├── html/
│   ├── index.html          # Web UI
│   ├── app.js              # UI logic
│   ├── style.css           # Dark theme
│   ├── swagger.html        # Swagger UI
│   └── openapi.json        # OpenAPI 3.0 spec
└── settings/
    ├── settings.json       # Default settings
    ├── events.json         # Declared VAPIX events
    └── default_rules.json  # Example rules loaded on first run
```

## License

MIT
