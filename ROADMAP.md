# Roadmap: candidate features

These are proposals, not commitments. Each one needs evidence that users want it
before it is built. Shipped OS 13 work is described in the
[README](README.md#roadmap) and [OS13_MIGRATION.md](OS13_MIGRATION.md).

Facts marked *tested* were checked on a P3288-LV running AXIS OS 12.11.77 with
Event Engine 1.10.1.

## 0. Mask secrets in rule output

**Done for exports in 1.10.2.** Rule and settings exports leave passwords,
tokens, webhook URLs and credential headers out unless the user chooses
*Include secrets* (`include_secrets=1` in the API). Saving or importing a rule
with the `__REDACTED__` placeholder keeps the stored value. The SMTP password
is no longer returned by `GET /settings`.

**Still open:** `GET /rules?id=` returns secrets unmasked, because the rule
editor loads rules through it. The restore-on-save logic already handles the
placeholder, so masking it only needs the editor to show masked fields as
"(unchanged)". This has to be done before an MCP server exposes `get_rule`.

## 1. MCP server

**Problem:** writing rules by hand means learning the rule JSON schema, every
trigger and action field, and the template variables.
[AI_RULE_GUIDE.md](AI_RULE_GUIDE.md) exists so an assistant can do this, but
the assistant still has to read a long document and call the REST API without
help.

**Idea:** expose Event Engine's rule model as MCP tools. An assistant could
then answer requests such as "when motion is detected at night, start
recording and send me a Telegram message". It would discover the available
triggers and actions on that particular camera, build the rule, validate it,
and create it.

**Why here and not in a generic camera MCP:** existing on-camera MCP ACAPs
([EdgeMCP-for-Axis](https://github.com/kotyzap/EdgeMCP-for-Axis),
[acap-mcp-server](https://github.com/jrutanen/acap-mcp-server)) wrap VAPIX.
They don't know Event Engine's rule model, so they can't validate or explain
its rules. This server would only cover Event Engine.

**Proposed tools:**

| Tool | Kind | Backed by |
|------|------|-----------|
| `engine_status` | read | `GET /engine` |
| `list_triggers` / `list_actions` | read | `GET /triggers`, `GET /actions` (schemas) |
| `list_device_events` | read | `GET /acapevents` |
| `list_rules` / `get_rule` | read | `GET /rules` (needs masking, see 0) |
| `recent_events` | read | `GET /events?limit=&rule=` |
| `explain_rule` | read | plain-language summary of a rule's JSON |
| `validate_rule` | read | new: `POST /rules?action=validate`, checks without saving |
| `create_rule` | write | `POST /rules`; created **disabled** by default |
| `update_rule` / `set_rule_enabled` | write | `POST /rules?id=` |
| `delete_rule` | write | `DELETE /rules` |
| `test_action` | write | `POST /actions`; may cause physical effects |
| `fire_rule` | write | `POST /fire` |
| `get_variables` / `set_variable` | read / write | `/variables` |

**Validation is the main engine work.** `validate_rule_json()` in
[main.c](app/main.c) already runs on every save, but it only checks structure.
*Tested:* it rejects unknown trigger, condition and action types, an empty
`actions` list and an invalid `trigger_logic`. It accepts and saves all of
these:

- a `telegram` action without `bot_token` and `chat_id`;
- an `mqtt_publish` action without `topic`;
- a `schedule` trigger without `schedule_type`, or with the cron `99 * * * *`;
- a `rule_fired` trigger pointing at a rule that doesn't exist;
- a `variable_compare` condition on a variable that doesn't exist;
- a misspelled field (`mesage` instead of `message`);
- an unclosed template (`{{trigger.nonexistent`).

These errors only show up later as log warnings, or not at all. Assistants will
make exactly these mistakes, so `validate_rule` needs per-type required fields,
unknown-field warnings, cron and template parsing, and reference checks.
Errors should name the path, for example `actions[1].topic is required`; today
the message is `actions 1 has unknown type 'telegramm'`. The same checks
improve saves from the web UI and the REST API, so this is worth doing even
without MCP.

**Safety:**

- Read-only by default. Write tools need an explicit setting in the Event
  Engine UI.
- New rules are created disabled. Enabling a rule, `test_action`, and
  `fire_rule` require `confirm: true`. These can trigger sirens, PTZ moves,
  relays, or outbound messages.
- Never return secrets in tool output (depends on 0).
- Log every write tool call to the event log with the tool name.

**Transport and authentication:** Event Engine's endpoints are FastCGI entries
in `httpConfig`. The manifest schema (v2.2.0) allows only `admin`, `operator`
or `viewer` access there, so Apache always demands camera credentials (Digest
on this camera) before the app sees a request. Many MCP clients only support
bearer tokens or OAuth. Options:

- **Host-side stdio bridge** that talks Digest to the existing REST API. No
  camera changes except the validate endpoint, works with any MCP client, and
  is the cheapest way to run the first test.
- **On-camera endpoint with a bearer token.** `reverseProxy` entries may use
  `anonymous` access, and Event Engine already serves `alertStream` through a
  reverse proxy to its own HTTP server. An MCP path there could check a token
  generated in the Event Engine UI. It would be reachable without camera
  credentials, so the token check is the only protection. Verify that Apache
  forwards the `Authorization` header to the proxy target.
- Rely on [VAPX](https://github.com/Hex29A/vapx)-style tooling.

**Overlap to check:** VAPX's MCP server and EdgeMCP may add generic action-rule
tools. Neither knows Event Engine's model today. Re-check before building.

**First test:** build the stdio bridge with the read tools plus
`validate_rule`. Ask an assistant to write ten realistic rules from the
[use cases](use-cases). Measure how many validate on the first try, how many
are correct after one round of fixes, and how many pass validation but are
still wrong. Build the on-camera endpoint only if the bridge proves useful.

## 2. Recording Stalled trigger

**Problem:** a camera can look online while its recording is missing, stalled,
or much shorter than expected.

**Trigger:** fires when a continuous recording stops advancing for longer than
a configured period. Clears when new recording data appears again. Limit it to
continuous recording: event-triggered recordings have no expected state.

**Must not fire:**

- while continuous recording is turned off;
- while storage is being formatted;
- after an intentional stop by an Event Engine Recording action.

**What the camera already offers (*tested*):** these events can already be used
through the `vapix_event` trigger and `vapix_event_state` condition:

- `Storage/Disruption` (`disk_id`, `disruption`);
- `Storage/Alert` (`disk_id`, `alert`, `wear`, `overall_health`, `temperature`);
- `Device/HardwareFailure/StorageFailure`;
- `Storage/Recording` (`recording`: whether a recording is ongoing);
- `Storage/RecorderStatus` (`name=continuous`, `status`).

Document a use case built from these first. It may cover a pulled or failed SD
card without new code.

**Open question: how to detect "not advancing".** *Tested:* `record/list.cgi`
reports an ongoing continuous recording with an empty `stoptime` and
`recordingstatus="recording"`, so it shows no progress. Candidates are disk
used space from the Edge Storage disk APIs (fails when retention cleanup keeps
usage flat) or the time of the newest recorded data. Find a reliable signal
before committing to the trigger.

**Overlap to check:** AXIS Image Health Analytics covers image quality only. A
VMS may already raise recording-health alerts. The value is in catching
edge-storage failures at sites without a VMS.

**First test:** on supported models, stop recording, pull the SD card, and fill
the storage. Verify each case produces one event, not repeated noise.

## 3. Clock Integrity trigger

**Problem:** unsynchronized or jumping clocks make footage and events hard to
correlate across cameras.

**Trigger:** fires on any of:

- loss of NTP sync;
- an offset above a threshold;
- a wall-clock step larger than N seconds, detected by comparing
  `CLOCK_REALTIME` against `CLOCK_MONOTONIC`.

Expose the offset as a numeric value so existing thresholds and hold durations
apply. This trigger only reports and never changes time settings.

**Data source (*tested*):** the camera declares no NTP or time event. The NTP
API (`/axis-cgi/ntp.cgi`, method `getNTPInfo`, version 1.5) returns `synced`,
`timeOffset` in seconds and `timeToNextSync`, so the trigger has to poll it.
Existing rules can't do this today: `http_check` sends no request body and no
credentials, and only compares values for equality.

**First test:** block the NTP server, then step the clock manually. Measure
detection delay and confirm the trigger clears when sync returns.

## Suggested order

1. **Secret masking:** exports done in 1.10.2; mask `GET /rules?id=` before
   MCP.
2. **Stricter validation and `POST /rules?action=validate`:** useful on its
   own, and the core of the MCP work.
3. **MCP server** as a host-side bridge with read tools plus `validate_rule`.
   Move it onto the camera only if the first test shows demand.
4. **Recording Stalled** and **Clock Integrity:** add them if installers
   confirm the problems occur. Try a Recording Stalled use case built from the
   existing storage events before writing a new trigger.
