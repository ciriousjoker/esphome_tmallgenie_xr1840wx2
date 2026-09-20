# Changelog

## Unreleased

- Restore the last confirmed light state after an ESP32 restart instead of reporting the lamp as off.
- Stop issuing unsupported periodic state queries that only time out on the reference driver.
- Support the ESPHome 2026.9 framework validation and ESP-IDF component discovery APIs.

## v0.1.0 — 2026-07-15

- Initial ESPHome external-component release for the TmallGenie `XR 18-40WX2` LED driver.
- Exposes verified on/off control and reported power state.
- Includes experimental brightness, color-temperature, preset, AliGenie MainLight, timer, and KLD-12KEY mappings, clearly marked as unverified.
- Adds acknowledged command retries, state polling, automatic ESP-IDF configuration, key/address validation, and persistent Mesh sequence-range management.
