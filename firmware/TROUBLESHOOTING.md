# Troubleshooting

## Serial logging

Use:

```bash
pio device monitor -b 115200
```

Useful tags include `DIVAAN_CORE`, `AUDIO_REC`, `BACKEND_CLIENT`, `BATTERY_ADC`, `OTA_MGR`, and `SPEAKER_SDM`.

## Boot and hardware

### LCD is blank

Check GPIO 10, 8, 9, 16 and shared SPI GPIOs 12, 11, 13. The driver uses a 40 MHz LCD device and initializes the backlight high on GPIO 16.

### SD mount fails or assets are missing

The mount uses SD-SPI on GPIO 6 at a 20 MHz host limit and does not format a failed card. Verify filesystem, chip select, shared SPI wiring, exact `/sd` names, and raw RGB565 dimensions.

### Touch controls do not work

Screen touch uses CS GPIO 7 at 2 MHz and accepts raw coordinates from 200 to 3900. Verify XPT2046 wiring. The four capacitive inputs are GPIOs 1, 2, 3, and 5.

## Touch and UI

### Voice capture never starts

The active `touch_sensor_task()` directly reads pads 2 and 3. They must stay high for at least 350 ms, recording only starts in Home mode when Focus is not running, and release queues the upload request.

### Pad behavior differs from the reusable touch API

The current `main.c` does not call `touch_sensor_update_and_poll()`. It directly reads GPIOs and implements its own timing. Debug `touch_sensor_task()` for the active behavior.

## Audio

### Recording file is not created

The recorder component creates `/sd/rec` and opens `/sd/rec/voice.pcm`, but the current `main.c` does not initialize or start that component. A missing or unwritable SD card also cancels capture when the recorder API is used. Check for `Failed to create /sd/rec/voice.pcm` and verify whether another part of the firmware has created the file before upload.

### Backend receives an unexpected amount of audio

The recorder writes to its configured target, but `audio_worker` currently calls `backend_client_send_audio_sd("/sd/rec/voice.pcm", 16000 * 2 * 3, ...)`. Check actual file size and fixed upload length together when changing recording duration.

### No response audio

The backend client streams response bytes only for HTTP 200. Confirm HTTPS success, raw PCM response data, and GPIO 15 amplifier wiring. The speaker driver queues 512-byte packets to a playback task on core 1.

### Sound effect missing

Alarm and focus paths first read `/sd/sfx/alarm.pcm` and `/sd/sfx/focus_done.pcm`; fallback tones are generated if those files are unavailable.

## Battery

### Battery icon is stuck at the default

The driver starts at 7.50 V and samples no more often than every 1.5 seconds. ADC errors, invalid values outside 4.5..9.5 V, or incorrect divider wiring preserve the last state. Verify GPIO 14, ADC2 channel 3, the 100k/47k divider, and ground.

### Charging or critical state is unexpected

Charging is inferred solely from voltage `>= 8.20 V`. Critical is `< 6.60 V`. Bars use thresholds 8.10, 7.60, 7.20, and 6.60 V. There is no separate charger-status input.

## Network

### Wi-Fi repeatedly reconnects

The event handler retries with `esp_wifi_connect()`. Check source-defined credentials in `include/config.h`, signal strength, and serial logs. The display indicator becomes connected after `IP_EVENT_STA_GOT_IP`.

### Weather is stale or absent

Weather is fetched at most every 30 minutes from a hardcoded Open-Meteo URL. The parser only searches for `"temperature":`; failures leave the previous display string unchanged.

### Backend actions are not applied

`net_task` polls once per second and parses JSON `action`. It ignores empty and `NONE` values. Check exact forms `NOTIF:[app] message`, `SET_ALARM:<hour>:<minute>:<enabled>`, and the other action names in `ARCHITECTURE.md`.

### Phone media controls do not work

The current music UI calls `backend_client_send_media_cmd()` with `PREV`, `PLAY_PAUSE`, or `NEXT`. The separate `wifi_bridge` component defaults to `192.168.43.1:8080` but is not used by this UI path.

## OTA

### No update popup

The popup requires action `CHECK_OTA`, a successful manifest request, and a version different from `v1.2.0`. Confirm manifest fields `version`, `notes`, and `firmware_url`.

### OTA download fails

Check HTTPS reachability, certificate validity, firmware URL, available OTA slot, and use of the custom `partitions.csv`. The UI bar is animated and is not reliable download progress.

### No restart after update

The OTA manager calls `esp_restart()` only when `esp_https_ota()` returns `ESP_OK`. Inspect `OTA_MGR` logs for the error returned otherwise.

## Time and alarm

SNTP uses `pool.ntp.org`, then `app_main()` sets `TZ` to `IST-5:30`. Alarm checks use local time in the UI task. Settings load from `/sd/metrics/alarm.json`, then NVS namespace `storage`, then default to 07:30 enabled.

## Diagnostic limitations

- There is no automated hardware test suite.
- Backend service code and schemas are outside this repository.
- Relationship metric save/init functions are empty.
- OTA progress is animated rather than measured.
