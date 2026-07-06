# Release Notes

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
