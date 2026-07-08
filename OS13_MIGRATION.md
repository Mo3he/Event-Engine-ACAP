# AXIS OS 13 Migration Roadmap

Target: AXIS OS 13.0, scheduled September 2026. Preview firmware available April 2026.

**Status:** P0 (SDK + manifest) and P1 (recording API) are done as of v1.9.14, built against ACAP Native SDK `12.10.0`. Remaining: manual ACAP signing (P0), plus P2/P3 items below. Signing must be done through the Axis ACAP Portal and cannot be automated here.

---

## P0 - Required to install/run on OS 13

### 1. Recompile against OS 13 SDK - DONE

- **Why**: 64-bit `time_t` ABI break means existing `.eap` files won't install on OS 13. The device will rollback the upgrade if our ACAP is present.
- **Done**: `Dockerfile` bumped to `acap-native-sdk:12.10.0` on `ubuntu24.04` (from `1.15.1`/`ubuntu22.04`). Build verified.
- **GLib symbol gotcha (fixed)**: The SDK 12.x toolchain ships GLib 2.80. GLib 2.76+ redefines the `g_string_free()` macro to call `g_string_free_and_steal`, so a binary built with the new SDK fails at runtime on older firmware with `undefined symbol: g_string_free_and_steal`. Fixed by capping the GLib API in `app/Makefile`: `-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_66`. Verified absent from both `.eap` binaries via `readelf --dyn-syms`.
- **glibc symbol gotcha (fixed, validated on OS 11.11)**: The SDK 12.10 toolchain ships glibc 2.42, which bumped the symbol version of the termios calls `cfsetispeed()`/`cfsetospeed()` (used by the Modbus RTU/serial code in `modbus_pool.c`). This made the binary require `GLIBC_2.42`, so it installed but crash-looped on AXIS OS 11.x with `libc.so.6: version 'GLIBC_2.42' not found`. These were the *only* two symbols pulling in 2.42. Fixed with arch-conditional `.symver` pins in `modbus_pool.c` (aarch64 -> `@GLIBC_2.17`, armv7hf -> `@GLIBC_2.4`); behaviour is unchanged for standard baud rates. The binary now tops out at `GLIBC_2.34` and runs on AXIS OS 11.11 (verified on an AXIS P3265-LVE, FW 11.11.212).

### 2. Declare OS compatibility in manifest.json - DONE

- **Why**: OS 13 refuses to install or upgrade ACAPs that don't declare OS version compatibility.
- **Done**: Added `compatibleOsVersions: [{ "min": "11.11", "max": "13" }]` to `acapPackageConf.setup` (the field is `compatibleOsVersions`, not the earlier guessed `compatibleWith`). If only `max` is given, `acap-build` auto-injects `min` = the SDK baseline (`12.10.68`), which would needlessly block OS 12.0-12.10.67 devices (OS 11.x ignores the field entirely). Pinning `min` explicitly to `11.11` is honored and matches the real runtime floor (glibc 2.34, present since AXIS OS 11.11).

### 3. Manifest schema v2 - DONE

- **Why**: OS 13 enforces manifest schema v2.
- **Done**: `schemaVersion` bumped `1.5.0` -> `2.0.0`. Schema v2 changes that were required:
  - `vendorId` (10 hex chars) is now **required**.
  - `embeddedSdkVersion` is **no longer allowed** in `setup` and was removed.
  - `architecture` is required but auto-injected by `acap-build` from the build ARCH.
  - `httpConfig` / `reverseProxy` / `resources.dbus` remained valid unchanged.

### 4. ACAP signing - TODO (manual)

- **Why**: OS 13 only accepts signed ACAPs.
- **What**: Sign each built `.eap` through the Axis ACAP Portal (ties to vendor account / `vendorId`). Cannot be automated in this repo.

---

## P1 - Preserve recording functionality - DONE

### 5. Replace `record/record.cgi` + `record/stop.cgi` - DONE

- **Why**: Both endpoints are removed in OS 13.
- **Where**: `app/engine/actions.c` - `action_recording()`, `while_active_undo()` recording branch, `recording_stop_timeout_cb()`, and new continuous-recording helpers.
- **Done**: Capability-aware implementation. On products where `Properties.LocalStorage.ContinuousRecording=yes` (OS 13, and OS 12 products that support it) recordings use:
  - Start: `record/continuous/addconfiguration.cgi?diskid=<id>&eventid=eventengine[&options=streamprofile%3D<p>]` -> returns `profile` number (tracked like the old recording ID).
  - Stop: `record/continuous/removeconfiguration.cgi?profile=N`, or `continuous_remove_ours()` which lists profiles via `listconfiguration.cgi` and removes all Event Engine (`eventid=eventengine`) profiles on the disk.
  - Products without continuous-profile support fall back to the legacy `record.cgi`/`stop.cgi` path (unchanged), so one binary serves both OS 12 and OS 13.
  - `list.cgi` is still used for the legacy fallback (Axis cancelled the planned `list.cgi` change).
- **Gotcha (validated on hardware)**: unlike legacy `record.cgi`, `addconfiguration.cgi` **requires** the `options` parameter — omitting it returns HTTP 400. When no stream profile is configured we default to `options=videocodec%3Dh264`, which keeps the sensor's native resolution/frame rate.
- **Device limit**: `Properties.LocalStorage.NbrOfContinuousRecordingProfiles` is often `1`. If the single slot is already in use (e.g. a user-configured continuous recording), a start action fails with `Max number of configurations already configured` and is logged.
- **Verified** end-to-end on AXIS Q3538-LVE (FW 12.10.73): start creates an `eventid=eventengine` profile and an ongoing recording; stop removes the profile via `listconfiguration.cgi` + `removeconfiguration.cgi` and the recording is finalized on the SD card.
- **API docs**: [Edge Storage API](https://developer.axis.com/vapix/network-video/edge-storage-api/)

---

## P2 - User-facing issues (no code change, but users need guidance)

### 6. Camera tampering trigger migration

- **Why**: `MotionRegionDetector/Motion` event is removed. Any user rules using the camera tampering trigger will silently stop firing.
- **What**: Update docs/UI to recommend AXIS Image Health Analytics events as replacement. Consider adding a deprecation warning in the UI if the trigger type is `tnsaxis:CameraApplicationPlatform/MotionRegionDetector/Motion`.
- **Effort**: Low.

### 7. Remote camera HTTPS enforcement - DONE (v1.9.14)

- **Why**: Cameras factory-reset on OS 13 default to HTTPS-only. The `remote_host` feature previously only sent HTTP requests to remote cameras, which those devices reject.
- **Done**: Added a per-target `remote_https` boolean to remote-capable actions and conditions (UI checkbox "Use HTTPS"). When set, the remote curl helpers in `actions.c`/`conditions.c` build `https://` URLs and disable TLS peer/host verification to accept the self-signed certificates Axis cameras use by default. Left off, the transport stays HTTP, so mixed HTTP/HTTPS fleets both work. Verified on hardware: an overlay pushed from an OS 12.10 camera to an OS 11.11 camera over HTTPS succeeded.

---

## P3 - Pre-existing bugs to fix opportunistically (not OS 13 specific)

### 8. Replace non-thread-safe time functions

- **Why**: `localtime()` and `gmtime()` use a static buffer and are not thread-safe. HTTP handlers run in a thread pool, so these can race.
- **Where**:
  - `app/engine/scheduler.c` lines ~162, 205 - use `gmtime_r()` / `localtime_r()`
  - `app/engine/conditions.c` line ~46 - use `localtime_r()`
  - `app/engine/event_log.c` lines ~148, 157 - use `localtime_r()`
- `app/ACAP.c` and `app/engine/alert_stream.c` already use the `_r` variants correctly.
- **Effort**: Low.

---

## Order of work

1. **Done (v1.9.14)**: SDK bump (#1), GLib API cap + glibc symver pin (runs on OS 11.11-13), manifest `compatibleOsVersions` 11.11-13 + schema v2 + `vendorId` (#2, #3), recording API migration (#5), remote-host HTTPS toggle (#7). All verified on hardware (OS 11.11 + 12.10). Both `.eap` files build clean and are symbol-verified.
2. **April 2026 preview**: Validate on preview firmware.
3. **Before September 2026**: Sign the `.eap` files via the Axis ACAP Portal (#4).
4. **Opportunistic**: Fix non-thread-safe time functions (#8) in any upcoming release.
5. **Docs/UX**: Address the tampering trigger (#6) as part of any OS 13 release notes.

---

## Reference

- [AXIS OS 13 breaking changes overview](https://www.axis.com/for-developers/news/AXIS-OS-13-breaking-changes)
- [Full changes list](https://help.axis.com/en-us/axis-os#changes-in-axis-os-13)
- [Edge Storage API (recording replacement)](https://developer.axis.com/vapix/network-video/edge-storage-api/)
- OS 13.0 scheduled: September 2026
- Preview firmware: April 2026
