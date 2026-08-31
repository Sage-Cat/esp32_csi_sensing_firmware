# ESP32 CSI Sensing Firmware

`esp32_csi_sensing_firmware` is a standalone, reusable ESP32 firmware
implementation that emits CSI and bounded health telemetry. Its implementation
is `software/esp32-csi-sensing-firmware`.

The maintained implementation also exposes the bounded
`cws-firmware-control/1` measurement-rate protocol with correlated
prepare/apply/query/restore replies and boot/configuration epochs. Host tests
and clean ESP32-S3/C5 builds validate the implementation path; no physical
deployment result or scientific claim is implied.

This repository contains source, documentation, tests, and example
configuration profiles. It does not contain deployment credentials, captured
records, or scientific claims.

The repository is licensed under Apache License 2.0; see `LICENSE` and
`NOTICE`.

Run `PYTHONDONTWRITEBYTECODE=1 python3 scripts/check_public_tree.py` and
`PYTHONDONTWRITEBYTECODE=1 python3 scripts/validate_repository.py` for
standalone repository and public-release checks.
