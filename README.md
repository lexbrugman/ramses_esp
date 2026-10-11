**RAMSES_ESP** is a port of [evofw3](https://github.com/ghoti57/evofw3) to an Espressif esp32-s3 platform

The esp32-s3 interfaces to a TI CC1101 radio.

In addition to the USB serial interface provided by HGI80 and evofw3 based devices RAMSES_ESP also implements an MQTT interface to host applications.

This provides the ability to release the device from the need to be co-located with the system running the host application.  All the device needs is USB power. 

You can buy an Espressif [esp32-s3 + CC1101 USB stick](https://github.com/IndaloTech/ramses_esp/wiki/Hardware) here
https://indalo-tech.onlineweb.shop/

## Over-the-air updates

CI builds every push and pull request (see
[`firmware.yml`](.github/workflows/firmware.yml)), and publishes a **rolling
release** tagged `<branch>-latest` - overwritten on each build - that's the OTA
channel devices poll:

```
https://github.com/lexbrugman/ramses_esp/releases/download/main-latest/ramses_esp_esp32s3.bin
https://github.com/lexbrugman/ramses_esp/releases/download/main-latest/version.txt           # short commit SHA
```

`ramses_esp_<target>.bin` is the app-only image OTA flashes. The release also
holds a `ramses_esp_<target>_full.bin` for a first install over USB without
building from source - it includes the bootloader and partition table, so
flash it at offset 0x0:

```bash
esptool.py --chip esp32s3 write_flash 0x0 ramses_esp_esp32s3_full.bin
```

- Shortly after boot and every 6 hours the device fetches `version.txt` and
  reports it as the latest version - it does not install anything on its own.
  An install downloads and flashes the image only when `version.txt` differs
  from the running version.
- The build version (`FW_VERSION`) is the short commit SHA, so the running
  firmware's version always matches the commit it was built from. Local builds
  report `dev`.
- The channel comes from `FW_OTA_REPO` and `FW_OTA_CHANNEL`, which CI sets to
  the repo it's running in (`<owner>/<repo>`) and `<branch>-latest` for the
  branch being built - so a binary always tracks the repo and channel it came
  from. Flash a feature-branch build onto a test device and it keeps following
  that branch from then on, without touching production devices. Local builds
  and forks fall back to this repo's `main-latest`.

From the console (or published to the `RAMSES/GATEWAY/<device>/cmd/cmd` MQTT
topic):

| Command | |
| --- | --- |
| `ota check` | fetch the channel's version now instead of waiting for the next check |
| `ota start` | install the channel's version if it differs from the running one |
| `ota force` | install the channel's image even if it is the same version |
| `ota url <url>` | follow another channel until the next restart, e.g. `https://github.com/<owner>/<repo>/releases/download/<branch>-latest` |

### Home Assistant

When connected to MQTT the gateway registers itself with Home Assistant's MQTT
integration (discovery prefix `homeassistant`, configurable in menuconfig) as a
**RAMSES ESP `<device>`** device with:

| Entity | Type | Notes |
| --- | --- | --- |
| Firmware | update | titled with the OTA channel and linked to its release page; Install downloads and flashes the latest, with progress |
| Last restart | sensor | diagnostic, when the gateway last restarted |
| Check for updates | button | re-read the OTA channel's version now instead of waiting for the next check |
| Restart | button | reboot the ESP |

These sit alongside the `RAMSES/GATEWAY/<device>/...` topics host applications
use; the OTA state is published under `RAMSES/GATEWAY/<device>/ota/` and the
uptime for **Last restart** on `RAMSES/GATEWAY/<device>/uptime`.

The gateway has no reliable clock, so for **Last restart** it publishes its
uptime and Home Assistant converts that into a timestamp. It sends it only when
it connects and when Home Assistant restarts, so the entity's state changes
only on a restart.

Entities removed from the firmware disappear from Home Assistant on their own:
after connecting, the gateway deletes any retained discovery config under its
node ID (`ramses_esp_<device>`) that it didn't publish itself.
