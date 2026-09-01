# Event Engine - Use Cases & Templates

Some use cases for the ACAP Event Engine, each with worked examples and ready-to-import rule templates.

> **Import a template:** Open the Event Engine web UI → Rules tab → Import / use the API or paste the JSON into a POST to `/local/acap_event_engine/rules`.

---

## Use Case 1: Notifications & Data Transmission

Push alerts to people and push data to systems. The camera becomes an edge sensor node that detects conditions, formats messages, and delivers them over HTTP, MQTT, SMTP, or directly into time-series databases - no middleware required.

### 1.1 Environmental Sensor Data Transmission

**Scenario:** An Axis environmental sensor (thermometry, air quality, etc) periodically reads sensor values and transmits them to external systems. Two separate templates cover the two main approaches:

#### 1.1a - Power BI Real-Time Dashboard

Push sensor data directly to a Power BI streaming dataset via its REST API - the camera feeds the dashboard with no gateway or middleware in between.

**How it works:**

- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** action fetches the latest cached sensor data (temperature, humidity, CO₂) and injects the values as `{{trigger.FIELD}}` variables
- An **HTTP Request** action POSTs a JSON array to the Power BI streaming dataset endpoint - data appears instantly on a real-time Power BI tile

**Setup:** In Power BI, create a Streaming dataset (API type) with columns: `timestamp` (DateTime), `camera_serial` (Text), `camera_model` (Text), `temperature` (Number), `humidity` (Number), `co2` (Number). Copy the push URL into the template.

**Template:** [`templates/1.1a-sensor-data-powerbi.json`](templates/1.1a-sensor-data-powerbi.json)

#### 1.1b - Direct InfluxDB Ingestion (Grafana Dashboard)

Write time-series data points directly into InfluxDB with no middleware - the camera is the data pipeline.

**How it works:**

- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** action fetches the latest cached sensor data
- An **InfluxDB Write** action writes a data point with temperature, humidity, and CO₂ as fields and the camera serial as a tag - ready to query in Grafana

**Template:** [`templates/1.1b-sensor-data-influxdb.json`](templates/1.1b-sensor-data-influxdb.json)

#### 1.1c - MQTT to Home Assistant

Publish sensor data over MQTT using Home Assistant's topic conventions - sensors appear in HA with retained state updates every 60 seconds.

**How it works:**

- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** action fetches the latest cached sensor data
- An **MQTT Publish** action sends a retained JSON payload to `homeassistant/sensor/<serial>/environment/state` - HA picks up temperature, humidity, and CO₂ as sensor entities

**Template:** [`templates/1.1c-sensor-data-mqtt-homeassistant.json`](templates/1.1c-sensor-data-mqtt-homeassistant.json)

---

### 1.2 Multi-Platform Motion Alerts

**Scenario:** When motion is detected during office hours, send an alert with a camera snapshot to Slack, Microsoft Teams, and email simultaneously - so that security staff receive the notification on whichever platform they monitor.

**How it works:**

- A **Device Event** trigger subscribes to Motion Detection (active = true)
- A **Time Window** condition restricts firing to weekdays 08:00–18:00
- Three actions run in sequence: **Slack webhook**, **Teams webhook**, and **Email** - each with a template that includes the camera name, timestamp, and a snapshot attachment
- A **30-second cooldown** prevents alert floods from sustained motion

**Template:** [`templates/1.2-multi-platform-motion-alerts.json`](templates/1.2-multi-platform-motion-alerts.json)

---

### 1.3 Daily Activity Digest

**Scenario:** Instead of one notification per event, buffer all motion events throughout the day and send a single summary email at 18:00 showing every event with its timestamp - a daily activity report for facility managers.

**How it works:**

- A **Device Event** trigger subscribes to Motion Detection (active = true)
- A **Notification Digest** action buffers each event as one line (`{{time}} - Motion detected on {{camera.model}} ({{camera.serial}})`) and flushes via email every 86 400 seconds (24 hours)
- A second **schedule** trigger fires at 18:00 daily to ensure the digest sends even if no motion occurs (the flush still delivers whatever is buffered)

**Template:** [`templates/1.3-daily-activity-digest.json`](templates/1.3-daily-activity-digest.json)

---

## Use Case 2: Device Control

Automate camera hardware functions. The camera adjusts its own imaging, PTZ position, overlays, privacy masks, illumination, audio output, and physical hardware in response to events, schedules, or external commands - without any VMS or controller.

### 2.1 Sunrise/Sunset IR & Light Management

**Scenario:** At sunset, switch the IR cut filter to night mode and turn on the onboard IR illuminator at 80% intensity. At sunrise, switch back to day mode and turn the light off. No fixed schedule - the times follow the actual sun position at the camera's coordinates.

**How it works:**

- Two rules using **schedule** triggers with type `sunset` and `sunrise` (with latitude/longitude from the Settings → Location tab)
- The sunset rule runs two actions: **IR Cut Filter** (set to `night`) and **Light Control** (turn on `led0` at intensity 80)
- The sunrise rule mirrors it: **IR Cut Filter** (set to `day`) and **Light Control** (turn off `led0`)

**Templates:** [`templates/2.1a-sunset-night-mode.json`](templates/2.1a-sunset-night-mode.json) and [`templates/2.1b-sunrise-day-mode.json`](templates/2.1b-sunrise-day-mode.json)

---

### 2.2 Event-Driven Privacy Zones

A camera covers both a public area and a private office. Privacy masks need to follow a schedule for GDPR compliance, but must be overridable instantly during emergencies.

#### 2.2a/b - Scheduled Privacy (Business Hours)

**Scenario:** During business hours (Mon–Fri 08:00–18:00), enable a privacy mask over the office window for GDPR compliance. Outside business hours, disable the mask so security can monitor the full scene.

**How it works:**

- Two rules with **schedule** triggers - one fires at 08:00 weekdays, the other at 18:00 weekdays
- The morning rule runs a **Privacy Mask** action that enables the mask named `"Office Window"`
- The evening rule disables the same mask
- Both rules use a **Variable Compare** condition on `system.privacy_mode` to avoid toggling if an emergency override is active

**Templates:** [`templates/2.2a-privacy-mask-enable.json`](templates/2.2a-privacy-mask-enable.json) and [`templates/2.2b-privacy-mask-disable.json`](templates/2.2b-privacy-mask-disable.json)

#### 2.2c/d - Emergency Override

**Scenario:** When a fire alarm, intrusion alarm, or building management system signals an emergency, immediately disable all privacy masks for full scene visibility - regardless of schedule. When the emergency is cleared, restore normal operation.

**How it works:**

- An **MQTT Message** trigger listens on `building/emergency/+` (payload `"active"` or `"cleared"`), with an **HTTP Webhook** as a fallback trigger for systems that can't publish MQTT
- The emergency rule sets `system.privacy_mode` to `"override"` (which blocks the scheduled rules from re-enabling masks), disables the privacy mask, and displays a warning overlay
- The clear rule sets `system.privacy_mode` back to `"normal"` and re-enables the mask - the next scheduled rule resumes normal cycling

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

### 2.4 Motion → Audio Clip

**Scenario:** When motion is detected, play a pre-uploaded audio clip through the camera speaker — useful as an on-site deterrent (warning tone or voice message) or to alert staff in the same building without sending a remote notification.

**How it works:**

- A **Device Event** trigger subscribes to Motion Detection (active = true)
- An **Audio Clip** action plays the selected clip by name or ID (clips are uploaded via the camera's Audio section under Settings)
- A **10-second cooldown** prevents the clip repeating on sustained motion

**Template:** [`templates/2.4-motion-audio-clip.json`](templates/2.4-motion-audio-clip.json)

---

### 2.5 Scheduled Wiper

**Scenario:** An outdoor camera accumulates condensation or debris on its lens cover overnight. Run the wiper automatically every morning at 07:00 to restore a clear view before the working day begins — no manual intervention required.

**How it works:**

- A **schedule** trigger fires daily at 07:00, every day of the week
- A **Wiper** action starts wiper run 1 (the primary wiper service)
- Adjust the time or restrict to weekdays only by changing the `days` array in the template

**Template:** [`templates/2.5-schedule-wiper.json`](templates/2.5-schedule-wiper.json)

---

## Use Case 3: Security & Surveillance Automation

Orchestrate full security responses on the edge. Combine detection triggers with conditions and multi-step action chains to implement intrusion response, access control, and occupancy monitoring - running entirely on the camera with no cloud dependency.

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

**Scenario:** A camera monitoring a secure door uses I/O input from a card reader or push button. During business hours, a valid input opens the door via I/O output (electric strike), starts a short recording, and logs the access. Outside business hours, only an alert is sent - no door release.

**How it works:**

- An **IO Input** trigger on port 1 (rising edge) detects the card reader/button press
- A **Time Window** condition checks Mon–Fri 08:00–18:00
- If the condition passes (business hours): **IO Output** (port 2 active for 5 s - door strike), **Recording** (start, 15 s duration), **Increment Counter** (increment `door_access`)
- A separate rule handles the after-hours case: same IO Input trigger, inverted time condition (i.e., *not* in business hours is achieved by a different time window 18:00–08:00), and a **Telegram** alert action: "After-hours access attempt at {{time}} on {{camera.model}}"

**Templates:** [`templates/3.2a-access-control-business-hours.json`](templates/3.2a-access-control-business-hours.json) and [`templates/3.2b-access-control-after-hours-alert.json`](templates/3.2b-access-control-after-hours-alert.json)

---

### 3.3 Occupancy Monitoring with Escalation

**Scenario:** A retail store uses AOA to count people. When occupancy exceeds the limit (e.g., 20 people), display a warning overlay. If it exceeds a critical threshold (e.g., 30 people), send a Slack alert to the store manager and activate the door indicator light via I/O.

**How it works:**

- Two rules at different thresholds sharing the same AOA scenario
- **Warning rule**: Schedule trigger (every 30 s) → **AOA Get Counts** (scenario 1) → **AOA Occupancy** condition (`human >= 20`) → **Overlay Text** ("Occupancy: {{aoa_human}} - LIMIT APPROACHING" on channel 1, duration 35 s)
- **Critical rule**: Schedule trigger (every 30 s) → **AOA Get Counts** (scenario 1) → **AOA Occupancy** condition (`human >= 30`) → **Slack webhook** ("CRITICAL: {{aoa_human}} people in store - capacity exceeded") + **IO Output** (port 1 active for 35 s - door indicator)

**Templates:** [`templates/3.3a-occupancy-warning.json`](templates/3.3a-occupancy-warning.json) and [`templates/3.3b-occupancy-critical.json`](templates/3.3b-occupancy-critical.json)

---

## Use Case 4: Speaker Display

Drive the built-in screen on Axis speaker-display devices (e.g. C1710) directly from rules. Data can come from three sources: **MQTT messages** pushed by a broker, **HTTP webhooks** POSTed by any system, or **direct polling** of another Axis device via `vapix_query` — no VMS or middleware required. The display becomes a live information board updated in real time.

### 4.1 Live Queue Ticket Display

**Scenario:** A service desk or healthcare waiting room uses a ticketing system. Whenever the next ticket is called, the ticketing system publishes an update over MQTT and the display instantly shows the current ticket number and remaining queue length — no screen controller, no separate dashboard app.

**How it works:**

- An **MQTT Message** trigger subscribes to `queue/reception/update`
- The payload is expected as JSON: `{"current":"A047","waiting":3}` — fields are available as `{{trigger.current}}` and `{{trigger.waiting}}`
- A **Speaker Display** action shows "NOW SERVING / A047 / 3 people waiting" on a dark-blue background, held for 5 minutes (or until the next update overwrites it)
- Zero cooldown so every ticket call updates the display immediately

**Setup:** Configure your ticketing system or a Node-RED / Home Assistant automation to publish to `queue/reception/update` with the JSON payload above. Adjust the topic to match your environment.

**Template:** [`templates/4.1-queue-ticket-display.json`](templates/4.1-queue-ticket-display.json)

---

### 4.2 Air Quality Monitor

An environmental sensor (or any MQTT-capable air quality device) publishes CO₂, PM2.5, temperature, and humidity readings continuously. Three rules keep the display accurate and safe at all times.

#### 4.2a - Live Data Dashboard

**Scenario:** The display always shows the latest sensor readings so occupants can see current air quality at a glance. When readings are within safe limits, the display shows a calm green dashboard updated in real-time.

**How it works:**

- An **MQTT Message** trigger subscribes to `sensors/airquality/data` (JSON payload: `{"co2":750,"pm25":8,"temperature":21.5,"humidity":45}`)
- A **Variable Compare** condition checks `system.airquality_alert != active` so the dashboard does not overwrite an active critical alert
- A **Speaker Display** action renders the four values on a green background, replacing the previous reading every time a new message arrives

**Template:** [`templates/4.2a-air-quality-display.json`](templates/4.2a-air-quality-display.json)

#### 4.2b - Critical Alert Override

**Scenario:** When CO₂ rises above 1 000 ppm or PM2.5 exceeds 35 μg/m³, the publishing system flags the reading as critical. The display immediately switches to a red scrolling alert that stays on screen until explicitly cleared — staff cannot miss it.

**How it works:**

- An **MQTT Message** trigger fires on `sensors/airquality/status` with payload `critical`
- A **Set Variable** action sets `system.airquality_alert = active`, which suppresses the normal dashboard rule (4.2a)
- A **Speaker Display** action shows a red scrolling warning with `while_active` enabled — it persists until the 4.2c clear rule fires
- A **60-second cooldown** prevents the alert re-triggering while already active

**Template:** [`templates/4.2b-air-quality-alert.json`](templates/4.2b-air-quality-alert.json)

#### 4.2c - All Clear / Resume Dashboard

**Scenario:** Once ventilation brings CO₂ and PM2.5 back within safe limits, the sensor system publishes `clear`. The alert is dismissed, the `system.airquality_alert` variable is reset, and the live dashboard resumes automatically.

**How it works:**

- An **MQTT Message** trigger fires on `sensors/airquality/status` with payload `clear`
- A **Set Variable** action resets `system.airquality_alert = inactive`
- A **Speaker Display** action briefly shows a green "Air Quality Normal" confirmation for 15 seconds — after which the next data message from 4.2a takes over

**Setup (4.2a–c):** Configure your air quality gateway or sensor to publish JSON readings to `sensors/airquality/data` and threshold status (`critical` / `clear`) to `sensors/airquality/status`. Adjust topic paths and threshold logic in your gateway to match your sensor model and acceptable limits.

**Template:** [`templates/4.2c-air-quality-clear.json`](templates/4.2c-air-quality-clear.json)

---

### 4.3 Direct Axis Sensor → Speaker Display

**Scenario:** An Axis speaker-display (e.g. C1710) polls an Axis environmental sensor directly — no MQTT broker, no gateway, no middleware. Every 60 seconds the display pulls CO₂, temperature, and humidity readings straight from the remote sensor's ONVIF event stream and updates its screen.

**How it works:**

- A **schedule** trigger fires every 60 seconds
- A **Device Event Query** (`vapix_query`) action targets the remote Axis sensor (IP, user, password) and queries the `Environment / AirQuality` event topic — the latest readings are injected as `{{trigger.CO2}}`, `{{trigger.Temperature}}`, and `{{trigger.Humidity}}`
- A **Speaker Display** action renders the values on a green background with a 65-second duration (slightly longer than the poll interval so the display never goes blank between updates)

**Setup:** Replace `remote_host`, `remote_user`, and `remote_pass` in the template with the IP and credentials of your Axis air quality sensor. The sensor must support `tns1:Environment / tnsaxis:AirQuality` events (e.g. any Axis device with an environmental sensor module).

**Template:** [`templates/4.3-direct-sensor-air-quality-display.json`](templates/4.3-direct-sensor-air-quality-display.json)

---

### 4.4 Webhook-Driven Speaker Display

**Scenario:** A building management system, sensor gateway, or automation platform pushes air quality readings to the speaker display over HTTP — no MQTT broker required. Each POST instantly updates the screen with fresh data.

**How it works:**

- An **HTTP Webhook** trigger listens for POSTs to `/local/acap_event_engine/fire?token=airquality-display-update` with a JSON payload: `{"co2":750,"pm25":8,"temperature":21.5,"humidity":45}` — fields become `{{trigger.co2}}`, `{{trigger.pm25}}`, etc.
- A **Speaker Display** action renders the four values on a green background, held for 5 minutes (or until the next webhook POST overwrites it)
- Zero cooldown so every POST updates the display immediately

**Setup:** Configure your sensor gateway or BMS to HTTP POST to:

```text
POST http://<speaker-ip>/local/acap_event_engine/fire?token=airquality-display-update
Content-Type: application/json

{"payload":{"co2":750,"pm25":8,"temperature":21.5,"humidity":45}}
```

Change the token in the template to a unique value for your deployment.

**Template:** [`templates/4.4-webhook-air-quality-display.json`](templates/4.4-webhook-air-quality-display.json)

---

## Use Case 5: System Management

Manage the state and health of the engine itself. These templates control the engine's operational mode, maintain counters and variables, and provide observability through MQTT — the building blocks that other rules depend on.

### 5.1 Arm / Disarm via MQTT

**Scenario:** A central security panel, Home Assistant automation, or Node-RED flow sends an MQTT command to arm or disarm the camera. Other rules (e.g. 3.1 Perimeter Intrusion Response) gate their actions on `system.armed = true` — so a single MQTT publish instantly activates or deactivates all guarded rules across the camera.

**How it works:**

- Two rules listen on `cameras/{{camera.serial}}/arm` — one for payload `arm`, one for `disarm`
- Each rule runs a **Set Variable** action (`system.armed = true` or `false`) and optionally a **Syslog** entry for the audit trail
- Publish the topic from any MQTT client — the camera updates its state within the next event cycle

**Templates:** [`templates/5.1a-arm-system-via-mqtt.json`](templates/5.1a-arm-system-via-mqtt.json) and [`templates/5.1b-disarm-system-via-mqtt.json`](templates/5.1b-disarm-system-via-mqtt.json)

---

### 5.2 Daily Counter Reset

**Scenario:** Rules that count door access events, motion detections, or other occurrences store their totals in variables (e.g. `door_access`, `motion_count`). Reset all daily counters to zero at midnight so reports always reflect the current day — no accumulated drift across days.

**How it works:**

- A **schedule** trigger fires daily at 00:00
- A sequence of **Set Variable** actions resets each counter (`door_access = 0`, `motion_count = 0`) in a single rule pass
- An optional **Syslog** action logs "Daily counters reset" for the audit trail

**Template:** [`templates/5.2-daily-counter-reset.json`](templates/5.2-daily-counter-reset.json)

---

### 5.3 MQTT Heartbeat

**Scenario:** External monitoring systems (Node-RED, Home Assistant, Zabbix, Grafana) need to know if the camera and engine are alive. Publish a lightweight status payload every 60 seconds — if the topic goes silent, the monitoring system raises an alert.

**How it works:**

- A **schedule** trigger fires every 60 seconds
- An **MQTT Publish** action sends `{"serial":"{{camera.serial}}","model":"{{camera.model}}","time":"{{timestamp}}","armed":"{{system.armed}}"}` to `cameras/{{camera.serial}}/heartbeat`
- Monitoring software subscribes to the topic and alerts on silence — a timeout of ~120 seconds catches any unreachable device

**Template:** [`templates/5.3-mqtt-heartbeat.json`](templates/5.3-mqtt-heartbeat.json)

---

### 5.4 Maintenance Mode via MQTT

**Scenario:** Engineers arrive on-site for maintenance. A single MQTT publish activates maintenance mode — suppressing all alert rules that carry a `system.maintenance = false` condition. When work is done, a second MQTT command restores normal operation. The live-stream overlay confirms the active state at a glance.

**How it works:**

- Two rules listen on `cameras/{{camera.serial}}/maintenance` — one for payload `enable`, one for `disable`
- The enable rule sets `system.maintenance = true` and adds an **Overlay Text** ("MAINTENANCE MODE ACTIVE" on channel 1) so the live stream makes the state visible
- The disable rule sets `system.maintenance = false` and logs the restoration to syslog
- Any alert rule that should be suppressed during maintenance needs a **Variable Compare** condition: `system.maintenance = false`

**Templates:** [`templates/5.4a-maintenance-mode-enable.json`](templates/5.4a-maintenance-mode-enable.json) and [`templates/5.4b-maintenance-mode-disable.json`](templates/5.4b-maintenance-mode-disable.json)

---

### 5.5 Remote Rule Control via MQTT

**Scenario:** A central automation system (Home Assistant, Node-RED, SCADA) needs to dynamically switch which rules are active on the camera — enabling seasonal alert rules in winter, disabling them in summer, or activating a high-security rule set outside business hours. A single MQTT publish targets any rule by ID.

**How it works:**

- Two rules listen on `cameras/{{camera.serial}}/rules/<target_rule_id>/enable` — one for payload `enable`, one for `disable`
- Each rule runs an **Enable / Disable Rule** action that directly enables or disables the target rule and logs the change to syslog
- Replace `REPLACE_WITH_TARGET_RULE_ID` in each template with the actual UUID of the rule you want to control (copy it from the rule editor's rule list)
- The change takes effect immediately — the target rule's triggers are registered or unregistered in the same event cycle

**Setup:** Replace the placeholder rule ID in the imported templates with the UUID from the target rule's settings panel. The MQTT topic embeds the rule ID so each enable/disable pair is scoped to a single rule — deploy multiple pairs to control multiple rules independently.

**Templates:** [`templates/5.5a-enable-rule-via-mqtt.json`](templates/5.5a-enable-rule-via-mqtt.json) and [`templates/5.5b-disable-rule-via-mqtt.json`](templates/5.5b-disable-rule-via-mqtt.json)

---

## Use Case 6: Cross-Device Automation

Event Engine can check conditions on — and act on — **remote Axis devices** over the network. This lets a single Event Engine instance orchestrate a multi-camera or multi-device workflow: detect on camera A, validate state on camera B, act on device C.

### 6.1 I/O Input → Remote I/O State Condition → Speaker Display

**Scenario:** A door opens (an I/O input rising edge on the Event Engine camera), but you only want to raise an alarm if a second camera's door sensor is also active — preventing false alarms from the first sensor glitching. When both are active, show an alert on a remote speaker display.

**How it works:**

- An **I/O Input** trigger fires on port 1 rising edge
- An **I/O State** condition checks port 1 on a *remote* camera (e.g. `192.168.1.101`) — rule proceeds only if that sensor is also active
- A **Speaker Display** action shows a scrolling alert on a *remote* speaker display (e.g. `192.168.1.102`) for 15 seconds

**Setup:** Set the remote IP, username, and password in both the condition and action blocks. Adjust port numbers to match your camera wiring.

**Template:** [`templates/6.1-cross-device-io-condition-speaker-display.json`](templates/6.1-cross-device-io-condition-speaker-display.json)

---

### 6.2 AOA Detection → Remote AOA Occupancy Condition → Alert

**Scenario:** Camera A detects a person via AOA, but only raises an alert if camera B also currently counts at least one person (confirming the event is real, not a far-field false positive). The response includes a speaker display on a remote device and an MQTT alert.

**How it works:**

- An **AOA Scenario** trigger fires when scenario 1 detects an object
- An **AOA Occupancy** condition checks a *remote* camera (e.g. `192.168.1.101`) — passes only if occupancy > 0
- A **Speaker Display** action shows a 10-second warning on a remote speaker display
- An **MQTT Publish** action sends `{"camera":"…","event":"aoa_scenario","time":"…"}` to an alert topic

**Setup:** Set remote IPs, credentials, and scenario IDs to match your cameras. Adjust MQTT topic and broker settings under Settings → MQTT.

**Template:** [`templates/6.2-cross-device-aoa-condition-alert.json`](templates/6.2-cross-device-aoa-condition-alert.json)

---

### 6.3 Remote Sensor Data Relay

**Scenario:** Camera A (running Event Engine) periodically queries environmental sensor data from a remote Camera B and publishes it over MQTT with formatted decimal values — no Event Engine installation needed on Camera B.

**How it works:**

- A **Schedule** trigger fires every 60 seconds
- A **Device Event Query** action with a remote target queries Camera B's air quality event data (temperature, humidity, CO₂) via the VAPIX event API
- An **MQTT Publish** action sends the sensor values as a JSON object, using the `|N` format specifier to round decimals (e.g. `{{trigger.Temperature|2}}` → `20.35`)

**Setup:** Set Camera B's IP, username, and password in the Device Event Query action. Click **Load Events** to browse available events and select the correct one. Adjust the MQTT topic and broker settings under Settings → MQTT.

**Template:** [`templates/6.3-remote-sensor-data-relay.json`](templates/6.3-remote-sensor-data-relay.json)

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
| 2.4 | `2.4-motion-audio-clip.json` | Device Control | Motion → play audio clip (deterrent or on-site alert) |
| 2.5 | `2.5-schedule-wiper.json` | Device Control | Daily 07:00 → run camera wiper |
| 3.1 | `3.1-perimeter-intrusion-response.json` | Security | Human + armed + after-hours → full response |
| 3.2a | `3.2a-access-control-business-hours.json` | Security | Card reader → door release + recording |
| 3.2b | `3.2b-access-control-after-hours-alert.json` | Security | Card reader after hours → Telegram alert |
| 3.3a | `3.3a-occupancy-warning.json` | Security | Occupancy ≥ 20 → warning overlay |
| 3.3b | `3.3b-occupancy-critical.json` | Security | Occupancy ≥ 30 → Slack + IO output |
| 4.1 | `4.1-queue-ticket-display.json` | Speaker Display | MQTT ticket update → live queue number on display |
| 4.2a | `4.2a-air-quality-display.json` | Speaker Display | MQTT sensor data → live CO₂/PM2.5/temp/humidity dashboard |
| 4.2b | `4.2b-air-quality-alert.json` | Speaker Display | MQTT "critical" → red scrolling alert, suppress dashboard |
| 4.2c | `4.2c-air-quality-clear.json` | Speaker Display | MQTT "clear" → dismiss alert, resume live dashboard |
| 5.1a | `5.1a-arm-system-via-mqtt.json` | System Management | MQTT "arm" → set system.armed = true + syslog |
| 5.1b | `5.1b-disarm-system-via-mqtt.json` | System Management | MQTT "disarm" → set system.armed = false + syslog |
| 5.2 | `5.2-daily-counter-reset.json` | System Management | Midnight → reset daily counters to zero |
| 5.3 | `5.3-mqtt-heartbeat.json` | System Management | Every 60 s → publish status heartbeat to MQTT |
| 5.4a | `5.4a-maintenance-mode-enable.json` | System Management | MQTT "enable" → activate maintenance mode + overlay |
| 5.4b | `5.4b-maintenance-mode-disable.json` | System Management | MQTT "disable" → deactivate maintenance mode |
| 5.5a | `5.5a-enable-rule-via-mqtt.json` | System Management | MQTT "enable" → enable target rule by ID + syslog |
| 5.5b | `5.5b-disable-rule-via-mqtt.json` | System Management | MQTT "disable" → disable target rule by ID + syslog |
| 6.1 | `6.1-cross-device-io-condition-speaker-display.json` | Cross-Device | I/O input → remote I/O state condition → remote speaker display |
| 6.2 | `6.2-cross-device-aoa-condition-alert.json` | Cross-Device | AOA detection → remote AOA occupancy condition → speaker display + MQTT |
| 6.3 | `6.3-remote-sensor-data-relay.json` | Cross-Device | Schedule → remote sensor query → MQTT publish with `\|N` formatting |
| 4.3 | `4.3-direct-sensor-air-quality-display.json` | Speaker Display | Direct sensor poll → speaker display (no broker) |
| 4.4 | `4.4-webhook-air-quality-display.json` | Speaker Display | HTTP webhook → speaker display |
| 7.1a | `7.1a-motion-count-increment.json` | Advanced | Motion → increment counter |
| 7.1b | `7.1b-motion-count-escalate.json` | Advanced | Counter threshold → Slack + run_rule escalation |
| 7.2 | `7.2-compound-trigger-tailgate.json` | Advanced | Door I/O AND motion within 8s → tailgate alert |
| 7.3 | `7.3-night-only-recording.json` | Advanced | Motion + night condition → recording |
| 7.4 | `7.4-snapshot-upload-ftp-fallback.json` | Advanced | HTTP snapshot upload with FTP on_failure fallback |
| 7.5 | `7.5-acap-watchdog-cron.json` | Advanced | Cron → ACAP watchdog via vapix_event_state + acap_control |
| 7.6 | `7.6-paging-scheduled-announcement.json` | Advanced | Cron → paging console announcement |
| 7.7 | `7.7-vms-event-bridge.json` | Advanced | Manual trigger + webhook → fire VAPIX event for VMS |
| 7.8 | `7.8-security-audit-logger.json` | Advanced |  rule_fired trigger + condition_logic OR → audit log |
| 7.9 | `7.9-network-gated-alert.json` | Advanced | http_check condition → motion alert only when endpoint reachable |

---

## Use Case 7: Advanced Patterns

Templates demonstrating advanced Engine capabilities: compound triggers, counter-driven logic, dynamic scheduling, evidence failover, ACAP lifecycle management, and VMS integration.

### 7.1 Counter-Driven Escalation

**Scenario:** Low-level motion detection is normal, but 50 events in a single day suggests something unusual — a crowd, a fault, or a threat. Count every motion event throughout the day and escalate automatically when the threshold is crossed.

**How it works:**

- **7.1a:** A **Device Event** trigger (motion active) fires the **Increment Counter** action on every detection, accumulating `motion_count`
- **7.1b:** A **Counter Threshold** trigger watches `motion_count >= 50`; a **Counter Compare** condition double-checks before acting; a **Slack** alert fires once per day, optionally **Run Rule** chains to a downstream response rule
- Pair with template **5.2** to reset `motion_count` to zero at midnight

**Templates:** [`templates/7.1a-motion-count-increment.json`](templates/7.1a-motion-count-increment.json) and [`templates/7.1b-motion-count-escalate.json`](templates/7.1b-motion-count-escalate.json)

---

### 7.2 Compound Trigger — Tailgate Detection

**Scenario:** A door I/O sensor triggers frequently for legitimate access events. Motion detection fires frequently on its own. Neither signal alone is meaningful — but if both occur within 8 seconds of each other, that strongly indicates someone followed someone else through the door without badging.

**How it works:**

- `trigger_logic: "AND"` and `trigger_window: 8` require both an **I/O Input** (door rising edge) and a **Device Event** (motion active) to fire within 8 seconds
- A **Time Window** condition restricts to after-hours (18:00–08:00)
- Actions: **Recording** (30s), **Telegram** alert with snapshot, **IO Output** (alarm relay 5s)

**Template:** [`templates/7.2-compound-trigger-tailgate.json`](templates/7.2-compound-trigger-tailgate.json)

---

### 7.3 Night-Only Recording

**Scenario:** A camera should only record motion events during darkness — daytime recordings are unneeded and consume storage. Rather than using a fixed time window, use the sun's actual position so the schedule adapts to sunrise/sunset automatically across the year.

**How it works:**

- A **Device Event** trigger fires on motion detection
- A **Day/Night** condition (`state: "night"`) passes only when the sun is below the horizon, using the camera's configured latitude/longitude
- A **Recording** action starts a 30-second clip, with a live-stream overlay showing the detection time

**Template:** [`templates/7.3-night-only-recording.json`](templates/7.3-night-only-recording.json)

---

### 7.4 Snapshot Evidence with FTP Fallback

**Scenario:** Critical evidence snapshots must be preserved even when the primary upload server is unreachable. An `on_failure` action chain automatically reroutes to FTP backup storage if the HTTP POST fails for any reason.

**How it works:**

- AOA human detection (armed + after-hours) triggers a **HTTP Request** with `attach_snapshot: true`, posting a JSON body including the base64 image
- If the request fails (curl error or non-2xx status), the `on_failure` array runs: **FTP Upload** to backup storage + **Send Syslog** warning
- The failure path is transparent — no logic branching needed in the rule itself

**Template:** [`templates/7.4-snapshot-upload-ftp-fallback.json`](templates/7.4-snapshot-upload-ftp-fallback.json)

---

### 7.5 ACAP Service Watchdog

**Scenario:** A critical ACAP application (analytics, access control, etc.) must stay running. Poll its VAPIX event state every 10 minutes; if it reports as inactive, restart it, restore a known-good device parameter, and publish an alert.

**How it works:**

- A **cron** schedule trigger fires every 10 minutes (`*/10 * * * *`)
- A **Device Event State** condition checks the ACAP's running event; the condition passes (and actions run) only when the ACAP is NOT active
- Actions: **ACAP Control** (start), **Set Device Parameter** (restore to known-good value), **Send Syslog** (warning), **MQTT Publish** (monitoring alert)

**Setup:** Replace the `event_key` with the actual VAPIX event path of your ACAP, and `package` with its package name (e.g. `com.axis.objectanalytics`).

**Template:** [`templates/7.5-acap-watchdog-cron.json`](templates/7.5-acap-watchdog-cron.json)

---

### 7.6 Paging Console Scheduled Announcement

**Scenario:** An Axis C6110 paging console should broadcast a pre-recorded announcement at the top of every business hour — a safety reminder, a shift change call, or a building-wide notification — without any manual intervention.

**How it works:**

- A **cron** schedule trigger fires at `0 8-17 * * 1-5` (top of every hour, 08:00–17:00, Mon–Fri)
- A **Paging Console Execute** action triggers the pre-configured paging action by its UUID
- A **Send Syslog** action provides an audit trail

**Setup:** Replace `REPLACE_WITH_PAGING_ACTION_UUID` with the UUID of the action configured in the C6110 web interface.

**Template:** [`templates/7.6-paging-scheduled-announcement.json`](templates/7.6-paging-scheduled-announcement.json)

---

### 7.7 VMS Event Bridge

**Scenario:** Event Engine detects something (or receives an external command) and needs to notify a VMS (AXIS Camera Station, Milestone, Genetec). Rather than polling, the VMS subscribes to VAPIX events from the camera — the **Fire VAPIX Event** action emits the signal the VMS is waiting for.

**How it works:**

- A **manual** trigger (UI "Fire Now" button or `POST /fire` API call) or an **HTTP Webhook** from any external system starts the rule
- A **Fire VAPIX Event** action emits a `RuleFired` VAPIX event that the VMS is subscribed to
- An **MQTT Publish** action provides a parallel notification path
- Replace the webhook token with a unique value for your deployment

**Template:** [`templates/7.7-vms-event-bridge.json`](templates/7.7-vms-event-bridge.json)

---

### 7.8 Security Event Audit Logger

**Scenario:** Whenever a critical security rule fires, log it — but only if the system state warrants it. Using `condition_logic: OR`, the log fires if the system is armed OR if any motion activity has occurred today (counter > 0). Either condition is sufficient.

**How it works:**

- A **Rule Fired** trigger watches a specified security rule by UUID
- Two conditions with `condition_logic: OR`: **Variable Compare** (`system.armed = true`) and **Counter Compare** (`motion_count > 0`) — either passing is enough
- Actions: **Send Syslog** (warning level with variable interpolation) + **MQTT Publish** (to an audit topic)

**Setup:** Replace `REPLACE_WITH_SECURITY_RULE_ID` with the UUID of the security rule you want to audit (e.g. rule 3.1 Perimeter Intrusion Response).

**Template:** [`templates/7.8-security-audit-logger.json`](templates/7.8-security-audit-logger.json)

---

### 7.9 Network-Gated Motion Alert

**Scenario:** A camera in an isolated network segment sends alerts to a cloud endpoint. When connectivity drops, the alert queue fills with undeliverable requests. Using an **http_check** condition, the rule verifies the endpoint is UP before acting — silently skipping when it's unreachable rather than flooding failed queues.

**How it works:**

- A **Device Event** trigger fires on motion detection
- An **HTTP Check** condition makes a quick GET to `https://alerts.example.com/health` — the condition passes only if the response is HTTP 200 with body `ok`
- A **Time Window** condition restricts to office hours
- A **Slack Webhook** action fires only when both conditions pass

**Template:** [`templates/7.9-network-gated-alert.json`](templates/7.9-network-gated-alert.json)

### 7.10 Sparkplug B Telemetry to a SCADA Host

**Scenario:** An industrial or building-automation platform (Ignition, Honeywell
EBI, Cirrus Link, any Tahu-compatible host) expects MQTT Sparkplug B. The usual
answer is to put an industrial gateway between the Axis device and the Sparkplug
network. With Event Engine the device *is* the edge node, so no gateway is needed.

**How it works:**

- A **schedule** trigger fires every 30 seconds
- A **Device Event Query** action fetches the latest cached sensor values
- A **Sparkplug B Publish** action emits them as a protobuf `NDATA` on
  `spBv1.0/{group}/NDATA/{edge_node}` with the aliases from the birth certificate

**Setup:** Enable MQTT, then Settings → **Sparkplug B Edge Node**: set a Group ID
and declare the metrics `Camera/Temperature`, `Camera/Humidity` and `Camera/CO2`
(Double). The host discovers them from the `NBIRTH` automatically.

**Template:** [`templates/7.10-sparkplug-telemetry.json`](templates/7.10-sparkplug-telemetry.json)

### 7.11 Sparkplug B Command Handler

**Scenario:** The SCADA host needs to *control* the device, not just read from it
— for example triggering an announcement on a speaker from the operator console.

**How it works:**

- A **Sparkplug Command** trigger fires when the host writes the metric
  `Speaker/Play` via `NCMD`
- An **Audio Clip** action plays the clip named in `{{trigger.value}}`
- A **Set Variable** action records the command
- A **Sparkplug B Publish** action acknowledges by writing the result back as
  telemetry, so the host sees the command was carried out

**Setup:** Declare a `Camera/LastCommand` (String) metric in Settings. Any metric
name works for the trigger — it does not need to be declared, since inbound
commands are not part of the birth certificate.

**Template:** [`templates/7.11-sparkplug-command-handler.json`](templates/7.11-sparkplug-command-handler.json)

---

## Customisation

Every template is designed to be imported and then customised in the web UI:

- **Webhook URLs** - replace placeholder URLs with your actual Slack/Teams webhook endpoints
- **MQTT topics** - adjust the topic hierarchy to match your broker structure
- **InfluxDB** - set your server URL, database/bucket, and credentials
- **Telegram** - insert your bot token and chat ID
- **Email** - configure SMTP in Settings tab; only recipients need changing per rule
- **I/O ports** - match port numbers to your camera's physical wiring
- **AOA scenarios** - match scenario IDs to your configured analytics scenarios
- **PTZ presets / Guard tours / Privacy masks** - use names that match your camera configuration
- **Thresholds and timings** - adjust cooldowns, hold durations, and value thresholds to your environment
- **Sparkplug B** - declare every metric a template publishes in Settings first; undeclared names are rejected because they are absent from the birth certificate
- **Decimal formatting** - append `|N` to any numeric variable to control precision (e.g. `{{trigger.Temperature|2}}` for 2 decimal places)
