# Changelog

## Unreleased

## v0.3.0 - 2026-10-20

- Add an opt-in, account-free recovery pairing button for a configured lamp MAC, with a 60-second limit and authenticated application-key confirmation.
- Expose pairing progress and failures through an optional text sensor.
- Decode authenticated vendor advertising responses and use matching lamp on/off indications to complete pending commands.
- Keep the existing disabled SETUP mapping and automatic-startup behavior unchanged.
- Work around ESP-IDF 5.5.5 one-shot BLE advertising buffer ownership within this build, without editing the shared SDK cache.
- Add host tests for the public Bluetooth Mesh vector, handshake guards, timeout, authentication failures, and replay suppression.

## v0.2.0 - 2026-09-20

- Restore the last confirmed light state after an ESP32 restart instead of reporting the lamp as off.
- Stop issuing unsupported periodic state queries that only time out on the reference driver.
- Support the ESPHome 2026.9 framework validation and ESP-IDF component discovery APIs.

## v0.1.0 — 2026-07-15

- Initial ESPHome external-component release for the TmallGenie `XR 18-40WX2` LED driver.
- Exposes verified on/off control and reported power state.
- Includes experimental brightness, color-temperature, preset, AliGenie MainLight, timer, and KLD-12KEY mappings, clearly marked as unverified.
- Adds acknowledged command retries, state polling, automatic ESP-IDF configuration, key/address validation, and persistent Mesh sequence-range management.
