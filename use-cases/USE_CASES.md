# Event Engine — Use Cases & Templates

Three main use cases for the ACAP Event Engine, each with three worked examples and ready-to-import rule templates.

> **Import a template:** Open the Event Engine web UI → Rules tab → use the API or paste the JSON into a POST to `/local/acap_event_engine/rules`.

---

## Use Case 1: Notifications & Data Transmission

Push alerts to people and push data to systems. The camera becomes an edge sensor node that detects conditions, formats messages, and delivers them over HTTP, MQTT, SMTP, or directly into time-series databases — no middleware required.

### 1.1 Environmental Sensor Data Transmission

**Scenario:** An Axis camera with a connected environmental sensor (thermometry, air quality, etc) periodically reads sensor values and transmits them to external systems. Two separate templates cover the two main approaches:

#### 1.1a — Power BI Real-Time Dashboard

Push sensor data directly to a Power BI streaming dataset via its REST API — the camera feeds the dashboard with no gateway or middleware in between.

**How it works:**
- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** action fetches the latest cached sensor data (temperature, humidity, CO₂) and injects the values as `{{trigger.FIELD}}` variables
- An **HTTP Request** action POSTs a JSON array to the Power BI streaming dataset endpoint — data appears instantly on a real-time Power BI tile

**Setup:** In Power BI, create a Streaming dataset (API type) with columns: `timestamp` (DateTime), `camera_serial` (Text), `camera_model` (Text), `temperature` (Number), `humidity` (Number), `co2` (Number). Copy the push URL into the template.

**Template:** [`templates/1.1a-sensor-data-powerbi.json`](templates/1.1a-sensor-data-powerbi.json)

#### 1.1b — Direct InfluxDB Ingestion (Grafana Dashboard)

Write time-series data points directly into InfluxDB with no middleware — the camera is the data pipeline.

**How it works:**
- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** action fetches the latest cached sensor data
- An **InfluxDB Write** action writes a data point with temperature, humidity, and CO₂ as fields and the camera serial as a tag — ready to query in Grafana

**Template:** [`templates/1.1b-sensor-data-influxdb.json`](templates/1.1b-sensor-data-influxdb.json)

#### 1.1c — MQTT to Home Assistant

Publish sensor data over MQTT using Home Assistant's topic conventions — sensors appear in HA with retained state updates every 60 seconds.

**How it works:**
- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** action fetches the latest cached sensor data
- An **MQTT Publish** action sends a retained JSON payload to `homeassistant/sensor/<serial>/environment/state` — HA picks up temperature, humidity, and CO₂ as sensor entities

**Template:** [`templates/1.1c-sensor-data-mqtt-homeassistant.json`](templates/1.1c-sensor-data-mqtt-homeassistant.json)

---

### 1.2 Multi-Platform Motion Alerts

**Scenario:** When motion is detected during office hours, send an alert with a camera snapshot to Slack, Microsoft Teams, and email simultaneously — so that security staff receive the notification on whichever platform they monitor.

**How it works:**
- A **Device Event** trigger subscribes to Motion Detection (active = true)
- A **Time Window** condition restricts firing to weekdays 08:00–18:00
- Three actions run in sequence: **Slack webhook**, **Teams webhook**, and **Email** — each with a template that includes the camera name, timestamp, and a snapshot attachment
- A **30-second cooldown** prevents alert floods from sustained motion

**Template:** [`templates/1.2-multi-channel-motion-alerts.json`](templates/1.2-multi-channel-motion-alerts.json)

---

### 1.3 Daily Activity Digest

**Scenario:** Instead of one notification per event, buffer all motion events throughout the day and send a single summary email at 18:00 showing every event with its timestamp — a daily activity report for facility managers.

**How it works:**
- A **Device Event** trigger subscribes to Motion Detection (active = true)
- A **Notification Digest** action buffers each event as one line (`{{time}} — Motion detected on {{camera.model}} ({{camera.serial}})`) and flushes via email every 86 400 seconds (24 hours)
- A second **schedule** trigger fires at 18:00 daily to ensure the digest sends even if no motion occurs (the flush still delivers whatever is buffered)

**Template:** [`templates/1.3-daily-activity-digest.json`](templates/1.3-daily-activity-digest.json)

---

## Use Case 2: Device Control

Automate camera hardware functions. The camera adjusts its own imaging, PTZ position, overlays, privacy masks, and illumination in response to events, schedules, or external commands — without any VMS or controller.

### 2.1 Sunrise/Sunset IR & Light Management

**Scenario:** At sunset, switch the IR cut filter to night mode and turn on the onboard IR illuminator at 80% intensity. At sunrise, switch back to day mode and turn the light off. No fixed schedule — the times follow the actual sun position at the camera's coordinates.

**How it works:**
- Two rules using **schedule** triggers with type `sunset` and `sunrise` (with latitude/longitude from the Settings → Location tab)
- The sunset rule runs two actions: **IR Cut Filter** (set to `night`) and **Light Control** (turn on `led0` at intensity 80)
- The sunrise rule mirrors it: **IR Cut Filter** (set to `day`) and **Light Control** (turn off `led0`)

**Templates:** [`templates/2.1a-sunset-night-mode.json`](templates/2.1a-sunset-night-mode.json) and [`templates/2.1b-sunrise-day-mode.json`](templates/2.1b-sunrise-day-mode.json)

---

### 2.2 Event-Driven Privacy Zones

A camera covers both a public area and a private office. Privacy masks need to follow a schedule for GDPR compliance, but must be overridable instantly during emergencies.

#### 2.2a/b — Scheduled Privacy (Business Hours)

**Scenario:** During business hours (Mon–Fri 08:00–18:00), enable a privacy mask over the office window for GDPR compliance. Outside business hours, disable the mask so security can monitor the full scene.

**How it works:**
- Two rules with **schedule** triggers — one fires at 08:00 weekdays, the other at 18:00 weekdays
- The morning rule runs a **Privacy Mask** action that enables the mask named `"Office Window"`
- The evening rule disables the same mask
- Both rules use a **Variable Compare** condition on `system.privacy_mode` to avoid toggling if an emergency override is active

**Templates:** [`templates/2.2a-privacy-mask-enable.json`](templates/2.2a-privacy-mask-enable.json) and [`templates/2.2b-privacy-mask-disable.json`](templates/2.2b-privacy-mask-disable.json)

#### 2.2c/d — Emergency Override

**Scenario:** When a fire alarm, intrusion alarm, or building management system signals an emergency, immediately disable all privacy masks for full scene visibility — regardless of schedule. When the emergency is cleared, restore normal operation.

**How it works:**
- An **MQTT Message** trigger listens on `building/emergency/+` (payload `"active"` or `"cleared"`), with an **HTTP Webhook** as a fallback trigger for systems that can't publish MQTT
- The emergency rule sets `system.privacy_mode` to `"override"` (which blocks the scheduled rules from re-enabling masks), disables the privacy mask, and displays a warning overlay
- The clear rule sets `system.privacy_mode` back to `"normal"` and re-enables the mask — the next scheduled rule resumes normal cycling

**Templates:** [`templates/2.2c-privacy-mask-emergency-override.json`](templates/2.2c-privacy-mask-emergency-override.json) and [`templates/2.2d-privacy-mask-emergency-clear.json`](templates/2.2d-privacy-mask-emergency-clear.json)

---

### 2.3 PTZ Auto-Tracking with Guard Tour Resume

**Scenario:** A PTZ camera runs a guard tour continuously. When Axis Object Analytics detects a person in a specific scenario zone, pause the tour, move to a PTZ preset that centres that zone, hold for 30 seconds, then resume the guard tour.

**How it works:**
- An **AOA Scenario** trigger fires on human detection in scenario 1
- The action sequence: **Guard Tour** (stop tour `"Patrol Route"`), **PTZ Preset** (go to `"Zone A Close-up"`), **Delay** (30 seconds), **Guard Tour** (start tour `"Patrol Route"`)
- A **60-second cooldown** prevents interrupting the tour repeatedly if the person lingers

**Template:** [`templates/2.3-ptz-track-and-resume-tour.json`](templates/2.3-ptz-track-and-resume-tour.json)

---

## Use Case 3: Security & Surveillance Automation

Orchestrate full security responses on the edge. Combine detection triggers with conditions and multi-step action chains to implement intrusion response, access control, and occupancy monitoring — running entirely on the camera with no cloud dependency.

### 3.1 Perimeter Intrusion Response

**Scenario:** When a person is detected in a restricted zone after hours and the security system is armed, trigger a full response: start recording, activate the siren/strobe, switch an I/O output to trigger an external alarm panel, send a Telegram alert with a snapshot, and overlay "INTRUSION DETECTED" on the live stream.

**How it works:**
- An **AOA Scenario** trigger detects humans in the perimeter scenario
- Two conditions: **Time Window** restricting to after-hours (19:00–06:00 every day) and **Variable Compare** checking `system.armed` = `"true"`
- Five actions in sequence: **Recording** (start), **Siren / Light** (start profile `"Intrusion"`), **IO Output** (port 1 active, duration 30 s), **Telegram** (alert with snapshot), **Overlay Text** ("INTRUSION DETECTED" on channel 1, duration 60 s)
- A **120-second cooldown** so the first response completes before re-triggering

**Template:** [`templates/3.1-perimeter-intrusion-response.json`](templates/3.1-perimeter-intrusion-response.json)

---

### 3.2 Smart Access Control

**Scenario:** A camera monitoring a secure door uses I/O input from a card reader or push button. During business hours, a valid input opens the door via I/O output (electric strike), starts a short recording, and logs the access. Outside business hours, only an alert is sent — no door release.

**How it works:**
- An **IO Input** trigger on port 1 (rising edge) detects the card reader/button press
- A **Time Window** condition checks Mon–Fri 08:00–18:00
- If the condition passes (business hours): **IO Output** (port 2 active for 5 s — door strike), **Recording** (start, 15 s duration), **Increment Counter** (increment `door_access`)
- A separate rule handles the after-hours case: same IO Input trigger, inverted time condition (i.e., *not* in business hours is achieved by a different time window 18:00–08:00), and a **Telegram** alert action: "After-hours access attempt at {{time}} on {{camera.model}}"

**Templates:** [`templates/3.2a-access-control-business-hours.json`](templates/3.2a-access-control-business-hours.json) and [`templates/3.2b-access-control-after-hours-alert.json`](templates/3.2b-access-control-after-hours-alert.json)

---

### 3.3 Occupancy Monitoring with Escalation

**Scenario:** A retail store uses AOA to count people. When occupancy exceeds the limit (e.g., 20 people), display a warning overlay. If it exceeds a critical threshold (e.g., 30 people), send a Slack alert to the store manager and activate the door indicator light via I/O.

**How it works:**
- Two rules at different thresholds sharing the same AOA scenario
- **Warning rule**: Schedule trigger (every 30 s) → **AOA Get Counts** (scenario 1) → **AOA Occupancy** condition (`human >= 20`) → **Overlay Text** ("Occupancy: {{aoa_human}} — LIMIT APPROACHING" on channel 1, duration 35 s)
- **Critical rule**: Schedule trigger (every 30 s) → **AOA Get Counts** (scenario 1) → **AOA Occupancy** condition (`human >= 30`) → **Slack webhook** ("CRITICAL: {{aoa_human}} people in store — capacity exceeded") + **IO Output** (port 1 active for 35 s — door indicator)

**Templates:** [`templates/3.3a-occupancy-warning.json`](templates/3.3a-occupancy-warning.json) and [`templates/3.3b-occupancy-critical.json`](templates/3.3b-occupancy-critical.json)

---

## Template Summary

| # | Template File | Use Case | Description |
|---|---|---|---|
| 1.1a | `1.1a-sensor-data-powerbi.json` | Data Transmission | Sensor → Power BI streaming dataset (real-time dashboard) |
| 1.1b | `1.1b-sensor-data-influxdb.json` | Data Transmission | Sensor → InfluxDB for Grafana dashboards |
| 1.1c | `1.1c-sensor-data-mqtt-homeassistant.json` | Data Transmission | Sensor → MQTT to Home Assistant |
| 1.2 | `1.2-multi-platform-motion-alerts.json` | Notifications | Motion → Slack + Teams + Email with snapshot |
| 1.3 | `1.3-daily-activity-digest.json` | Notifications | Buffer events → daily email summary |
| 2.1a | `2.1a-sunset-night-mode.json` | Device Control | Sunset → IR night + illuminator on |
| 2.1b | `2.1b-sunrise-day-mode.json` | Device Control | Sunrise → IR day + illuminator off |
| 2.2a | `2.2a-privacy-mask-enable.json` | Device Control | 08:00 weekdays → enable privacy mask |
| 2.2b | `2.2b-privacy-mask-disable.json` | Device Control | 18:00 weekdays → disable privacy mask |
| 2.2c | `2.2c-privacy-mask-emergency-override.json` | Device Control | Emergency signal → disable masks + set override |
| 2.2d | `2.2d-privacy-mask-emergency-clear.json` | Device Control | Emergency cleared → restore masks + resume schedule |
| 2.3 | `2.3-ptz-track-and-resume-tour.json` | Device Control | AOA human → stop tour → PTZ → resume |
| 3.1 | `3.1-perimeter-intrusion-response.json` | Security | Human + armed + after-hours → full response |
| 3.2a | `3.2a-access-control-business-hours.json` | Security | Card reader → door release + recording |
| 3.2b | `3.2b-access-control-after-hours-alert.json` | Security | Card reader after hours → Telegram alert |
| 3.3a | `3.3a-occupancy-warning.json` | Security | Occupancy ≥ 20 → warning overlay |
| 3.3b | `3.3b-occupancy-critical.json` | Security | Occupancy ≥ 30 → Slack + IO output |

---

## Customisation

Every template is designed to be imported and then customised in the web UI:

- **Webhook URLs** — replace placeholder URLs with your actual Slack/Teams webhook endpoints
- **MQTT topics** — adjust the topic hierarchy to match your broker structure
- **InfluxDB** — set your server URL, database/bucket, and credentials
- **Telegram** — insert your bot token and chat ID
- **Email** — configure SMTP in Settings tab; only recipients need changing per rule
- **I/O ports** — match port numbers to your camera's physical wiring
- **AOA scenarios** — match scenario IDs to your configured analytics scenarios
- **PTZ presets / Guard tours / Privacy masks** — use names that match your camera configuration
- **Thresholds and timings** — adjust cooldowns, hold durations, and value thresholds to your environment
