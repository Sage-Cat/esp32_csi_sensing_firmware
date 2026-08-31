# Repository boundary

This standalone repository owns reusable firmware source, tests, example
configuration profiles, and its documented serial-control contract. It does
not accept deployment configuration, captured records, location information,
or publication status. `public-release.toml` defines the tracked-path boundary
enforced by `scripts/check_public_tree.py` before a public release.
