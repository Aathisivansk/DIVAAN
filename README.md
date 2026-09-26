# Divaan

Divaan is an AI robot companion composed of an ESP32-S3 device, a FastAPI cloud service, and a Flutter companion application. The device owns real-time interaction, display, touch, audio playback, local state, and OTA updates. The backend handles speech recognition, response generation, text-to-speech, shared companion state, and command queues. The Flutter application connects phone notifications and media controls to the backend and provides a companion UI.

## System Scope

| Area | Component | Primary responsibility | Main entry points |
| --- | --- | --- | --- |
| Embedded runtime | `firmware/` | Touch/display interaction, local modes, recording, playback, telemetry, OTA | `firmware/src/main.c`, ESP-IDF components |
| Cloud runtime | `backend/` | Audio orchestration, AI response generation, TTS, device/mobile coordination | `backend/api/index.py` |
| Companion client | `divaan_app/` | Flutter state/UI, Android notification listener, media dispatch | `divaan_app/lib/main.dart`, Android native bridge |

The current implementation is a working integration baseline. It still contains hardcoded deployment values, in-memory backend queues, and firmware recording initialization that needs completion before it can be treated as production-ready.

## End-to-End Architecture

```text
                         +----------------------+
                         |  Flutter Companion   |
                         |  UI + Android bridge |
                         +----------+-----------+
                                    |
             notifications/media    | HTTPS JSON polling and commands
                                    v
+-------------------+       +-------+---------------------+       +------------------+
|   ESP32-S3 robot  | ----> |       FastAPI backend       | ----> | External services|
|                   | PCM   |                             |       | Speech / Gemini  |
| touch + display   | <---- | /api/emo/chat: PCM + headers| <---- | Google TTS        |
| FreeRTOS tasks    | audio | actions, state, telemetry   |       +------------------+
| SD + battery + OTA|       +-----------------------------+
+-------------------+
```

### Voice request path

```text
Touch hold
   |
   v
[touch_task] --audio_req_t--> [audio_upload_queue] --> [audio_worker]
                                                        |
                                                        | raw PCM, 16 kHz,
                                                        | mono, 16-bit
                                                        v
                                             POST /api/emo/chat
                                                        |
                                                        v
                         PCM -> WAV -> Speech Recognition -> Gemini
                                                        |
                                                        v
                                      reply text + emotion -> TTS
                                                        |
                                                        v
                            decoded, filtered PCM response stream
                                                        |
                                                        v
                                               speaker_driver
```

The firmware separates input sampling, UI rendering, network work, and audio work so a blocking HTTPS request does not directly stall touch handling or LCD updates. The backend remains the latency-critical serial section of the voice path: recognition, LLM generation, TTS retrieval, decoding, and response streaming occur within one request.

## Runtime Responsibilities

### ESP32-S3 firmware

The firmware is built with PlatformIO and ESP-IDF for `esp32-s3-devkitc-1`.

| FreeRTOS task | Core | Priority | Responsibility |
| --- | ---: | ---: | --- |
| `audio_worker` | 0 | 5 | Upload recorded PCM, consume streamed response audio, play event sounds |
| `touch_task` | 1 | 5 | Sample four touch inputs, detect holds/taps/gestures, enqueue audio work |
| `ui_task` | 1 | 5 | Own LCD rendering, modes, alarms, focus timer, games, and sleep state |
| `net_task` | 0 | 4 | Poll backend actions, send telemetry, fetch weather, check OTA |

Key boundaries:

- `s_audio_upload_queue` transfers audio requests from input/UI code to `audio_worker`.
- `s_ui_event_queue` transfers network events to `ui_task`.
- `ui_task` owns display operations and the primary application mode state.
- The SD card stores animation frames, icons, sound effects, metrics, and recorded audio.
- OTA uses dual application partitions defined in `firmware/partitions.csv`.

The current active voice path uploads `/sd/rec/voice.pcm` with a fixed three-second byte count. The `audio_recorder` component exists, but `main.c` does not currently initialize and start it; recording-file creation is therefore an implementation gap to resolve before validating end-to-end voice latency.

### FastAPI backend

The backend exposes a small HTTP control plane alongside the audio endpoint.

| Endpoint | Direction | Purpose |
| --- | --- | --- |
| `POST /api/emo/chat` | Robot -> backend | Accept raw PCM and return response PCM with `X-Emo-Reply` and `X-Emo-Emotion` headers |
| `GET /api/emo/poll_action` | Robot polls | Deliver queued notifications, media updates, feeding, sleep, alarm, and OTA actions |
| `GET /api/emo/status` | App/device -> backend | Return companion metrics, alarm state, and media state |
| `POST /api/bot/sync_telemetry` | Robot -> backend | Update battery and relationship telemetry |
| `POST /api/mobile/media_sync` | App -> backend | Publish current phone media metadata |
| `POST /api/mobile/action` | App -> backend | Queue companion actions such as feed, love, sleep, and OTA check |
| `POST /api/bot/media_cmd` | Backend/device -> queue | Queue media commands for the phone |
| `GET /api/mobile/poll_media_cmd` | App polls | Deliver queued media commands to the companion app |
| `GET /api/device/manifest.json` | Firmware -> backend | Return OTA version, notes, and firmware URL |

The audio contract is raw PCM at 16 kHz, mono, 16-bit samples. The backend wraps it in WAV for speech recognition, calls Gemini for structured reply and emotion data, retrieves TTS audio, decodes it to PCM, applies audio processing, and returns an `application/octet-stream` response.

### Flutter companion application

`DivaanState` is the application state owner. It manages backend polling, metrics, media state, notification access, theme persistence, and remote commands. Android-specific behavior is isolated behind platform channels:

| Channel | Direction | Purpose |
| --- | --- | --- |
| `com.divaan.companion/native_bridge` | Flutter -> Android | Check notification access, open settings, dispatch media key events |
| `com.divaan.companion/notification_stream` | Android -> Flutter | Stream notification package, title, and text |

`DivaanNotificationListener` observes Android media notifications independently of the visible Flutter route, then synchronizes media metadata with the backend. The Flutter app can dispatch `play_pause`, `next`, and `previous` to the active Android media session.

## Repository Layout

```text
.
+-- firmware/                 ESP32-S3 PlatformIO/ESP-IDF project
|   +-- src/main.c            Application composition and runtime tasks
|   +-- components/            Hardware, audio, network, OTA components
|   +-- include/config.h       Build-time device configuration
|   +-- ARCHITECTURE.md        Task, queue, state, and data-flow detail
|   +-- API.md                 Firmware component interfaces
|   +-- SETUP.md               Hardware and deployment setup
|   +-- TROUBLESHOOTING.md     Device diagnosis
+-- backend/                  FastAPI service
|   +-- api/index.py           Routes, queues, audio pipeline, integrations
|   +-- requirements.txt       Python dependencies
|   +-- README.md              Backend API and pipeline reference
+-- divaan_app/               Flutter companion
    +-- lib/main.dart          Flutter UI and DivaanState
    +-- android/app/src/       Native Android bridge and notification service
    +-- pubspec.yaml           Dart dependencies
    +-- README.md              Companion architecture and sideloading
```

## Local Development

### 1. Start the backend

```powershell
cd backend
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
$env:GEMINI_API_KEY = "<your-key>"
uvicorn api.index:app --reload --port 8000
```

The backend reads `GEMINI_API_KEY` from the environment. The firmware URL and deployed backend URL are currently configured in firmware source/configuration, so point `EMO_BACKEND_BASE_URL` at the reachable backend before device testing.

### 2. Build and flash firmware

```powershell
cd firmware
pio run -e esp32-s3-devkitc-1
pio run -t upload -e esp32-s3-devkitc-1
pio device monitor -b 115200
```

Required hardware includes the ESP32-S3 board, 320x240 ST7789 display, touch inputs, microphone, speaker amplifier, MicroSD card, battery circuit, and Wi-Fi. See [firmware/SETUP.md](firmware/SETUP.md) before wiring or flashing.

### 3. Build the companion app

```powershell
cd divaan_app
flutter pub get
flutter build apk --release
```

Install the generated APK with ADB, then enable Divaan Companion under Android notification access. The app needs a reachable backend URL matching the current Flutter configuration.

## Configuration and Deployment Notes

| Concern | Current behavior | Engineering implication |
| --- | --- | --- |
| Secrets | Firmware contains compiled Wi-Fi/backend values; backend uses `GEMINI_API_KEY` | Move device credentials and deployment secrets to provisioning or a secure configuration path |
| Backend state | Metrics and command queues are process-local Python objects | Use durable/shared storage for multi-instance deployment or restart tolerance |
| CORS | Backend allows all origins and credentials | Restrict origins and review authentication before public deployment |
| Audio capture | Firmware upload path assumes `/sd/rec/voice.pcm` exists | Initialize the recorder and define recording duration/file lifecycle |
| OTA | Firmware uses a manifest and dual partitions; displayed progress is animated | Add signed artifacts, integrity validation, and measured progress |
| External APIs | Speech, Gemini, and TTS are synchronous stages in the request | Measure each stage and add timeouts, retries, and observability around latency |

## Recommended Study Order

The root README is the architecture index. Read the subsystem documents in this order:

1. [firmware/ARCHITECTURE.md](firmware/ARCHITECTURE.md) for task segregation, queues, state ownership, voice flow, and OTA behavior.
2. [firmware/API.md](firmware/API.md) for component interfaces and hardware boundaries.
3. [backend/README.md](backend/README.md) for route contracts, audio conversion, AI integration, TTS, and deployment concerns.
4. [divaan_app/README.md](divaan_app/README.md) for Flutter state management, Android platform channels, notification access, and media control.
5. [firmware/SETUP.md](firmware/SETUP.md) and [firmware/TROUBLESHOOTING.md](firmware/TROUBLESHOOTING.md) for physical setup and diagnosis.

## Validation Baseline

There is no complete automated end-to-end test suite in the repository. Current component checks are:

```powershell
# Firmware compile
cd firmware
pio run -e esp32-s3-devkitc-1

# Flutter static analysis and tests
cd ..\divaan_app
flutter analyze
flutter test

# Backend import/startup check
cd ..\backend
python -c "from api.index import app; print(app.title or 'Divaan backend loaded')"
```

Hardware validation must additionally cover microphone capture, SD-card file creation, Wi-Fi reachability, HTTPS certificate handling, streamed speaker playback, action polling, Android notification access, and OTA rollback behavior.
