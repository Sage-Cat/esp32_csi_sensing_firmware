# ESP32 CSI Sensing Firmware

This ESP-IDF firmware associates with an existing 2.4 GHz Wi-Fi network,
continuously pings its gateway, and emits AP-to-sensor CSI over the board's
native USB Serial/JTAG connection. It does not change the AP, channel, SSID, or
any production-network setting.

The CSI path is based on Espressif esp-csi
examples/get-started/csi_recv_router at commit
8633d67152db2808f141cc1595970aa9cf406045, with the following implementation
extensions:

- persistent boot_epoch in NVS;
- stable firmware profile and device-label records;
- host-alignable monotonic timing/health heartbeats;
- AP BSSID/channel updates after reassociation;
- idempotent CSI reinitialization after association and a telemetry-only
  stalled-counter watchdog;
- probe-session restart after every renewed IP lease;
- explicit sequence and output-drop counters;
- explicit configured ping rate and internal chip-temperature telemetry;
- acknowledged runtime rate changes and deliberately logged reboot injection;
- 512-byte probe payloads matching the AQ-MC airtime model;
- runtime 0--50 Hz ping-rate commands on the shared serial protocol;
- bounded `cws-firmware-control/1` prepare/apply/query/restore control with
  boot/config epochs and idempotent command correlation;
- serialized CSI and heartbeat output for lossless chunk capture.

The initial hardware target is ESP32-S3 revision 0.2 with 16 MB flash and 8 MB
PSRAM. Builds are pinned to ESP-IDF v5.5.4.

The ESP32-C5 variant uses the same failure-aware envelope, persistent boot
epoch, runtime rate command, and deliberate reboot command. It emits the
official shorter C5 CSI metadata schema and records AGC/FFT gains. An example
device-class profile can set a preferred BSSID/channel; an empty preference
retains normal SSID-based AP selection.

## Build

Create sdkconfig.site from sdkconfig.site.example, restrict it to mode 0600,
select an example device-class defaults file, then activate the pinned IDF
environment and run (shown for the S3 example B profile):

    idf.py -B build-s3-example-b -D SDKCONFIG=sdkconfig.s3-example-b \
      -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.site;sdkconfig.s3-example-b.defaults;sdkconfig.rate-40.defaults" \
      set-target esp32s3
    idf.py -B build-s3-example-b -D SDKCONFIG=sdkconfig.s3-example-b \
      -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.site;sdkconfig.s3-example-b.defaults;sdkconfig.rate-40.defaults" \
      build

Use `sdkconfig.s3-example-a.defaults` and matching build/config paths for the
S3 example A profile. A separate build directory and generated sdkconfig for
each example profile prevent one device label from leaking into another image.
The C5 example profiles are `sdkconfig.c5-example-a.defaults` and
`sdkconfig.c5-example-b.defaults`.
Use `sdkconfig.rate-10.defaults`, `sdkconfig.rate-20.defaults`, or
`sdkconfig.rate-40.defaults` as the final defaults file for a rate-comparison
image. The selected rate is emitted in every profile and heartbeat record.

`firmware-manifest-c5-v1.0.0.json` is immutable provenance for earlier cited
C5 builds. Its abstract A/B labels and hashes describe those historical build
outputs; the credential-bearing binaries and generated sdkconfig files are not
distributed. The manifest is not a build recipe for the current neutral example
profiles and must not be rewritten with unverified hashes.

Do not publish sdkconfig.site, sdkconfig, or the built application image: the
site Wi-Fi credential is compiled into the firmware.

## Serial records

- CSI_PROFILE: firmware identity, persistent boot epoch, device label and MAC.
- CSI_DATA: raw CSI with sequence, RF metadata and ESP receive timestamp.
- CWSLAB_TIMING_HEARTBEAT: monotonic uptime, boot epoch, record/drop counters,
  association state and channel.

An external consumer can wrap each raw line with its own ingest time and
connection metadata.

Version 1.4.1 accepts `CWS_SET_PING_HZ 20` on the same USB serial channel. Zero
stops the generating ping session without disabling Wi-Fi or changing the
production network. Success emits `CWS_CONFIG_APPLIED ping_hz=20`; invalid or
failed requests retain the previous rate and emit `CWS_CONFIG_REJECTED`.
S3 version 1.4.2 and C5 version 1.0.1 keep the command-input task alive when
native USB Serial/JTAG temporarily reports EOF before the collector opens the
port, so both the legacy command and `cws-firmware-control/1` remain reachable
after unattended boot.
`CWS_REBOOT` emits an acknowledgement before a deliberate software reset, which
provides a reproducible fault injection without touching the board or cable.
Every heartbeat also reports CSI/probe reinitialization and failure counters,
CSI callback/filter diagnostics, ping success/timeout counters, and a bounded
CSI-stalled flag/counter.
Association always reapplies the CSI callback/configuration; three consecutive
connected heartbeats without CSI progress mark the capture stalled once; the
watchdog does not restart CSI, ping, or Wi-Fi during a long-running operation. The
probe session is rebuilt after every renewed IP lease so event-driven AP
reconnects do not retain a stale ping socket. CSI records are admitted only
when their source MAC is the current associated BSSID. Heartbeats refresh the
associated BSSID/channel from the station state so live channel changes remain
observable without reassociation.

The versioned endpoint is specified in [`docs/control-protocol-v1.md`](docs/control-protocol-v1.md).
It can change only the firmware-owned probe rate and reports the read-only
observed association state; it does not provide AP or channel control. Its
`config_epoch` resets on boot and is emitted in `CSI_PROFILE` and heartbeat
records without changing the established raw CSI CSV schema. A failed change
attempt restores the previous probe session when possible. If both the target
and restoration starts fail, the endpoint truthfully reports an inactive rate
and an `*-state-uncertain` reason; hardware postconditions remain externally
verified, not runtime/hardware validated by this implementation alone.
