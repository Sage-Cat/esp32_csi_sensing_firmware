# Contributing

Keep changes limited to reusable sensing-node behavior. Deployment
configuration and credentials belong outside the repository.

Validate with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 scripts/check_public_tree.py
PYTHONDONTWRITEBYTECODE=1 python3 scripts/validate_repository.py
sh software/esp32-csi-sensing-firmware/tests/run_control_protocol_tests.sh
```

Firmware changes also require a clean ESP32-S3 build with the pinned ESP-IDF
toolchain. Do not commit `build*/`, `managed_components/`, or generated
`sdkconfig` files. Public releases must satisfy `public-release.toml`.
