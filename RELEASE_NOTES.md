# Release Notes

## v1.9.15 — Sparkplug B edge node

Event Engine can now act as a **Sparkplug B edge node**, so an industrial or
building-automation host (Ignition, Honeywell EBI, Cirrus Link and other
Tahu-compatible systems) can consume device data and send commands back. The
usual answer to a Sparkplug requirement is to place an industrial gateway between
the Axis device and the Sparkplug network; with this release the device is the
edge node itself.

### Installation

The `signed_*.eap` packages are signed with the Axis ACAP signing service and
install normally on AXIS OS 12.10 and later.

Upgrading from **v1.9.13 or earlier** (which were unsigned) can fail with
"Couldn't install: app", because the vendor id in the manifest does not match the
previously installed version. Back up your rules first (Rules → Export), then
uninstall the old app before installing this one. Upgrading from v1.9.14 onwards
is a normal in-place install.

Each package is byte-for-byte the built artifact with an armored PGP signature
appended, so you can check one against its build:

```sh
head -c $(( $(stat -f%z signed_Event_Engine_1_9_15_aarch64.eap) - 849 )) \
    signed_Event_Engine_1_9_15_aarch64.eap | shasum -a 256
```

### What is new in 1.9.15

- **Sparkplug B Edge Node** settings section: group / edge node / device /
  primary host ids, a choice of Sparkplug 3.0 or 2.2, and a central metric list.
  Metrics can be bound to a device event (published automatically on change) or
  left unbound and driven from a rule.
- **Sparkplug B Publish** action — publish declared metrics as `NDATA`/`DDATA`
  with template-aware values.
- **Sparkplug Command** trigger — fires when a host writes a metric via
  `NCMD`/`DCMD`, injecting `{{trigger.metric}}` and `{{trigger.value}}`. This is
  what lets a SCADA system command the device, not just read from it.
- `GET /sparkplug` returns the live session state and every metric's current
  value; the Settings tab renders it as a table.
- `tools/sparkplug_host.py`, a dependency-light validation host that decodes the
  protobuf itself and checks birth-before-data, `seq` continuity, `bdSeq`
  matching and alias resolution.
- **Backup brokers.** The MQTT settings take a list of alternate brokers that are
  tried in order when the primary is unreachable, so a Sparkplug node is not tied
  to the availability of a single broker.

Protobuf encoding is implemented in-tree, so no new libraries are bundled.

### Correctness details

- `bdSeq` is persisted across restarts, so a late death certificate from a
  previous session cannot be mistaken for the current one.
- The death certificate is registered as the MQTT will **and** published
  explicitly on a graceful shutdown, because a clean MQTT disconnect suppresses
  the will.
- Sequence numbers are allocated in one place under a lock, so concurrent rules
  cannot interleave them.
- Metrics collected while the broker is unreachable are buffered and replayed on
  reconnect flagged as historical. The same applies while a configured primary
  host reports itself offline. A node with no primary host, or one whose host has
  never published `STATE`, always publishes, so a misconfigured host id cannot
  silently mute a device.
- Commands are accepted only on the node's own command topics, and a command
  delivered twice because a rule subscribes to an overlapping topic filter is
  acted on once.
- Both `Node Control/Rebirth` and `Device Control/Rebirth` are honoured.

### Fixes in 1.9.15

- **MQTT could stop reconnecting permanently after a settings change.** Changing
  the broker host or port closed the worker thread's socket from another thread.
  `select()` does not reliably notice that, and once the file descriptor number
  was reused elsewhere in the process the worker kept operating on an unrelated
  socket: it never reconnected and could write MQTT bytes into another
  connection. Reconnects are now requested through a wakeup pipe so the worker
  always closes its own socket, and a deliberate reconnect no longer waits out
  the failure backoff. Changing the username, password or client ID now also
  triggers a reconnect.
- MQTT: packets larger than the 4 KB receive buffer were parsed past the end of
  that buffer and left the stream unframed. Oversized packets are now drained
  safely.
- MQTT: `MQTT_Publish` measured payloads with `strlen`, which truncated any
  payload containing a NUL byte. Binary publishing is now supported.
- **Turning MQTT off left the broker connection open.** The worker only checked
  the enabled flag before starting a connection, never while one was running, so
  a disabled client kept pinging and receiving until something else dropped the
  socket.
- MQTT: the broker configuration was read by the worker thread while the settings
  thread overwrote it, which could pair a host from one configuration with the
  credentials or TLS setting of another. All access now goes through a lock and
  each connection attempt works from a private snapshot.
- MQTT: correcting the broker address while the client was backing off could take
  up to a minute to apply. Retry waits are now interrupted by a settings change.
- MQTT: a malformed remaining-length field could make the receive loop drain
  hundreds of megabytes and lose framing. It is now rejected.
- MQTT: packet assembly ignored allocation failures and could dereference a null
  pointer under memory pressure.
- Sparkplug: a failed replay of buffered samples discarded them anyway, losing
  exactly the data the buffer exists to protect.
- **Deleting a rule left its MQTT topic subscribed.** The broker kept delivering
  on that topic for the life of the app. Subscriptions are now released when a
  rule is deleted, disabled or edited, and are reference counted so one rule
  releasing a topic cannot cut off another rule or the Sparkplug command feed.
- **Posting a rule whose body carried an existing id created a second rule with
  the same id**, leaving get, update and delete ambiguous. Importing a backup on
  top of the rules it came from duplicated every one of them. Such a request now
  updates the existing rule.
- Sparkplug: `GET /sparkplug` reported a metric binding only as the flattened
  `source_path`, which the settings endpoint does not accept, so posting that
  response back silently dropped every binding. The response now also carries the
  writable `source` object, and a metric that arrives with `source_path` but no
  `source` is reported by name in the log instead of being quietly ignored.
- Settings: repeated no-op settings callbacks no longer cycle ACAP event
  subscriptions.

## v1.9.14-Signed - Signed release

- Packages are now signed with the Axis ACAP signing service and install normally on AXIS OS 12.10 and later.
- Vendor updated to `moshe@mohome.net` with the registered vendor ID.
- Upgrading from an earlier unsigned version can fail with "Couldn't install: app" (device log: "Vendor ID in manifest does not match the vendor ID of the previous version"). Back up your config, uninstall the old version, then install this one.

## v1.9.14 — AXIS OS 13 ready

### AXIS OS 13 compatibility

- Rebuilt against the AXIS OS 13 SDK (`acap-native-sdk:12.10.0`). The manifest was migrated to schema v2 with a `compatibleOsVersions` declaration of **11.11–13**.
- A single binary still runs on older firmware. Two toolchain symbol-version traps introduced by the newer SDK are pinned so the app loads on older AXIS OS:
  - GLib 2.76's `g_string_free_and_steal` (capped via the GLib API version in the Makefile).
  - glibc 2.42's `cfsetispeed` / `cfsetospeed` (pinned to each architecture's glibc baseline).
  - Verified running on **AXIS OS 11.11** and **12.10**.
- SD-card recording was migrated off the removed `record.cgi` / `stop.cgi` to the Edge Storage **continuous-recording API**, with automatic fallback to the legacy CGIs on products that don't support continuous-recording profiles.

### New

- **Per-device HTTPS.** Actions and conditions that target a remote camera now have a **Use HTTPS** toggle (`remote_https`). Required for AXIS OS 13 cameras that are HTTPS-only; the remote device's self-signed certificate is accepted. Leave it off for remotes still served over HTTP, so mixed fleets work either way.
- **Recording stream options.** The recording action gains an advanced **Stream Options** field (`options`), e.g. `resolution=1920x1080&fps=15` or `videocodec=h265`. Takes precedence over the stream profile. When both are blank it defaults to `videocodec=h264` at native resolution (the OS 13 continuous-recording API requires an options value).

### Fixes

- **`io_state` condition** now reads the actual `io/port.cgi` response format (`portN=active`/`portN=inactive`); it previously matched `active=yes`/`active=no`, which no firmware returns, so the condition always evaluated false.
- Light Control Flash strobe cache-bust fix (carried from the in-progress 1.9.14).

### Notes

- Production installs on **AXIS OS 13 require the `.eap` to be signed** via the Axis ACAP Portal (OS 13 rejects unsigned applications).
- Both `aarch64` and `armv7hf` builds are symbol-verified; runtime testing was performed on `aarch64` (OS 11.11 and 12.10).
