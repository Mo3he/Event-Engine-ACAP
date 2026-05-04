# AXIS OS 13 Migration Roadmap

Target: AXIS OS 13.0, scheduled September 2026. Preview firmware available April 2026.

---

## P0 - Required to install/run on OS 13

### 1. Recompile against OS 13 SDK
- **Why**: 64-bit `time_t` ABI break means existing `.eap` files won't install on OS 13. The device will rollback the upgrade if our ACAP is present.
- **What**: Bump the SDK base image in `Dockerfile` from `acap-native-sdk:1.15.1` to the OS 13-targeting SDK version (TBD, watch [ACAP SDK releases](https://github.com/AxisCommunications/acap-native-sdk)).
- **When**: Once Axis publishes the OS 13 compatible SDK (expected around the September 2026 release).
- **Effort**: Low - mechanical version bump, then verify build passes.

### 2. Add `compatibleWith` to manifest.json
- **Why**: OS 13 refuses to install or upgrade ACAPs that don't declare OS version compatibility.
- **What**: Add the `compatibleWith` field to `app/manifest.json`. Example:
  ```json
  "compatibleWith": {
    "minVersion": "12.0"
  }
  ```
  Exact field name/schema to confirm against OS 13 manifest schema docs when available.
- **When**: Can do now (OS 12 treats it as optional, OS 13 requires it).
- **Effort**: Trivial.

### 3. Manifest schema v2
- **Why**: OS 13 enforces manifest schema v2. We're on `schemaVersion: "1.5.0"`.
- **What**: Migrate `app/manifest.json` to schema v2 format. Confirm if `1.5.0` qualifies or if a `"2.x.x"` schema version is required.
- **When**: Verify with April 2026 preview firmware. May be bundled with item #1 (SDK bump usually updates the schema).
- **Effort**: Low-Medium depending on what changed in v2.

---

## P1 - Required to preserve recording functionality

### 4. Replace `record/record.cgi` + `record/stop.cgi`
- **Why**: Both endpoints are explicitly removed in OS 13.
- **Where**: `app/engine/actions.c` - the `record_local_*` action family (lines ~1120-1220, and `stop_active_recordings_from_xml` ~L66-L200).
- **What**: Migrate to the replacement APIs:
  - **Start recording**: Use the event-system recording action template (trigger-based), or `record/continuous/addconfiguration.cgi` for persistent continuous recording.
  - **Stop recording**: Must explicitly stop before remove. The new recording system assigns time-bounded IDs (max 24h), so a "stop all matching recordings" query will be needed.
  - Review `active_recordings[]` table and `recording_id[64]` tracking struct - the ID format may change.
- **API docs**: [Edge Storage API](https://developer.axis.com/vapix/network-video/edge-storage-api/)
- **When**: Before September 2026. Test on April 2026 preview firmware.
- **Effort**: Medium - the trigger/stop logic is self-contained but the new API shape is different.

---

## P2 - User-facing issues (no code change, but users need guidance)

### 5. Camera tampering trigger migration
- **Why**: `MotionRegionDetector/Motion` event is removed. Any user rules using the camera tampering trigger will silently stop firing.
- **What**: Update docs/UI to recommend AXIS Image Health Analytics events as replacement. Consider adding a deprecation warning in the UI if the trigger type is `tnsaxis:CameraApplicationPlatform/MotionRegionDetector/Motion`.
- **Effort**: Low.

### 6. Remote camera HTTPS enforcement
- **Why**: Cameras factory-reset on OS 13 default to HTTPS-only. The `remote_host` feature sends HTTP requests to remote cameras, which will be rejected.
- **What**: Consider adding an `https` toggle to the remote host settings so users can opt into HTTPS for remote VAPIX calls. Currently `actions.c` and `conditions.c` build `http://` URLs for remote hosts.
- **Effort**: Medium - need to handle TLS verification (likely need to disable cert verification for self-signed camera certs).

---

## P3 - Pre-existing bugs to fix opportunistically (not OS 13 specific)

### 7. Replace non-thread-safe time functions
- **Why**: `localtime()` and `gmtime()` use a static buffer and are not thread-safe. HTTP handlers run in a thread pool, so these can race.
- **Where**:
  - `app/engine/scheduler.c` lines ~162, 205 - use `gmtime_r()` / `localtime_r()`
  - `app/engine/conditions.c` line ~46 - use `localtime_r()`
  - `app/engine/event_log.c` lines ~148, 157 - use `localtime_r()`
- `app/ACAP.c` and `app/engine/alert_stream.c` already use the `_r` variants correctly.
- **Effort**: Low.

---

## Order of work

1. **Now (trivial)**: Add `compatibleWith` to manifest (#2).
2. **April 2026 preview**: Validate all of the above on preview firmware. Confirm schema v2 requirements (#3) and test recording API removal (#4).
3. **Before September 2026**: Complete #1, #3, #4 before OS 13 ships.
4. **Opportunistic**: Fix #7 in any upcoming release.
5. **Docs/UX**: Address #5 and #6 as part of any OS 13 release notes.

---

## Reference

- [AXIS OS 13 breaking changes overview](https://www.axis.com/for-developers/news/AXIS-OS-13-breaking-changes)
- [Full changes list](https://help.axis.com/en-us/axis-os#changes-in-axis-os-13)
- [Edge Storage API (recording replacement)](https://developer.axis.com/vapix/network-video/edge-storage-api/)
- OS 13.0 scheduled: September 2026
- Preview firmware: April 2026
