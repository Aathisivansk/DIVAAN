# Setup Guide

## Prerequisites

- ESP32-S3-DevKitC-1
- PlatformIO Core with the Espressif32 platform
- USB connection for flashing
- LCD, XPT2046-style touch controller, SD card, microphone, amplifier, four touch inputs, and battery divider
- Wi-Fi access to the configured network

## Hardware wiring

| Signal | GPIO |
| --- | ---: |
| Touch pad 1 | 1 |
| Touch pad 2 | 2 |
| Touch pad 3 | 3 |
| Touch pad 4 | 5 |
| Microphone ADC | 4 |
| Speaker / SDM output | 15 |
| SPI clock | 12 |
| SPI MOSI | 11 |
| SPI MISO | 13 |
| LCD CS | 10 |
| LCD DC | 8 |
| LCD reset | 9 |
| LCD backlight | 16 |
| Touch-controller CS | 7 |
| MicroSD CS | 6 |
| Battery ADC | 14 |

LCD, touch controller, and SD share SPI2 with separate chip selects. GPIO 4 is the microphone input; it is not the fourth touch pad in the current `config.h`.

## Configuration

Edit `include/config.h` before building. It contains the Wi-Fi credentials, backend URL, GPIO mapping, SD mount point, audio rate, maximum recording duration, NTP server, and timezone expression. The OTA manifest, weather URL, telemetry URL, and media gateway are hardcoded in the implementation. There is no runtime environment-variable configuration.

## SD-card preparation

The firmware uses `format_if_mount_failed = false`; prepare the filesystem externally.

```text
/sd/anims/listen/00.bin..07.bin
/sd/anims/think/00.bin..07.bin
/sd/anims/speak/00.bin..07.bin
/sd/anims/sleep/00.bin..07.bin
/sd/icons/dock/alarm.bin
/sd/icons/dock/focus.bin
/sd/icons/dock/ttt.bin
/sd/icons/dock/tap.bin
/sd/icons/dock/music.bin
/sd/sfx/alarm.pcm
/sd/sfx/focus_done.pcm
/sd/metrics/alarm.json
/sd/rec/voice.pcm
```

Animation frames are raw 88x100 RGB565. Dock icons are raw 40x40 RGB565. Audio files are raw PCM. Missing dock icons use text fallbacks; missing alarm/focus files use generated tones.

## Build, flash, and monitor

```bash
pio run -e esp32-s3-devkitc-1
pio run -t upload -e esp32-s3-devkitc-1
pio device monitor -b 115200
```

Combined command:

```bash
pio run -t upload -t monitor -e esp32-s3-devkitc-1
```

## Startup sequence

`app_main()` initializes NVS, creates UI/audio queues, initializes the display and SD card, precaches dock icons, loads alarm settings, initializes battery and speaker drivers, starts Wi-Fi and SNTP, sets the timezone, initializes the backend client, and creates the four worker tasks.

## OTA deployment

The custom partition table must remain in use because it defines `otadata`, `ota_0`, and `ota_1`. The manifest endpoint must return JSON containing `version`, `notes`, and `firmware_url`, for example:

```json
{
  "version": "v1.2.1",
  "notes": "Description shown in the update popup",
  "firmware_url": "https://example.invalid/firmware.bin"
}
```

The manifest version is compared with compiled `v1.2.0`. The firmware URL must be reachable over HTTPS using the ESP-IDF certificate bundle.

## Verification checklist

1. Build succeeds for `esp32-s3-devkitc-1`.
2. Serial output confirms Wi-Fi and task startup.
3. LCD renders the home face and battery icon.
4. SD card mounts and animation/icon files are present.
5. A middle-pad hold enters listening mode and release submits `/sd/rec/voice.pcm`; note that the current `main.c` does not invoke the recorder initialization/start API, so file creation must be verified separately.
6. Backend response audio is heard through the amplifier.
7. LCD touch opens the dock and application modes.
8. `CHECK_OTA` produces an update popup when the manifest version differs.
