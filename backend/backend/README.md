# Divaan Cloud API Backend Reference

Technical reference for the Divaan Cloud API server. The service is implemented in `api/index.py` and exposes a FastAPI application as `api.index:app`.

## Runtime Stack

| Component | Role |
|---|---|
| FastAPI | REST API framework and request routing |
| Python 3.10+ | Supported runtime |
| Uvicorn ASGI | Production/development ASGI server |
| Google Gemini 2.5 Flash | Generates short, emotion-aware JSON replies |
| Google Speech Recognition via `SpeechRecognition` | Converts the wrapped WAV request audio to text |
| Google Translate TTS | Converts the generated reply text to speech |
| Miniaudio | Decodes TTS audio and produces signed 16-bit, 16 kHz, mono PCM |
| HTTPX | Async HTTP client for Gemini and Google TTS |

The API currently enables permissive CORS (`*`) for origins, methods, and headers. Add authentication and restrict CORS before exposing a production deployment to untrusted clients.

## Base URL and Conventions

- Local base URL: `http://localhost:8000`
- All routes below are relative to the base URL.
- JSON responses use `application/json`.
- `POST /api/emo/chat` consumes raw audio bytes, not JSON or multipart form data.
- FastAPI parameters declared as function arguments are query parameters. This applies to `media_cmd`, `notify`, and `action`.
- The service stores metrics and pending queues in process memory. Restarting the worker clears pending commands, pending actions, and metric changes.

## REST API Reference

| Method | Route | Purpose | Request Payload/Params | Response Format |
|---|---|---|---|---|
| `GET` | `/api/emo/status` | Return service status, pet metrics, and current media state. | None. | JSON: `{ "status": "online", "affection": number, "hunger": number, "energy": number, "pats": number, "feeds": number, "chats": number, "media": { "title": string, "artist": string, "is_playing": boolean, "source": string } }` |
| `POST` | `/api/mobile/media_sync` | Update the currently playing media and enqueue a `SONG:` action for the bot. | JSON body: `{ "title": string, "artist": string, "is_playing": boolean, "source": string }`. Defaults: `title="No Track"`, `artist=""`, `is_playing=true`, `source="player"`. Title and artist are truncated to 36 and 28 characters. | `200` JSON: `{ "status": "ok" }`. Invalid/unreadable JSON: `400` JSON: `{ "error": string }`. |
| `POST` | `/api/bot/media_cmd` | Queue a media command for the mobile client. | Query parameter: `cmd` (string, required), for example `?cmd=PLAY`. The value is uppercased and trimmed. | JSON: `{ "status": "dispatched", "command": string }` |
| `GET` | `/api/mobile/poll_media_cmd` | Retrieve and remove the oldest queued media command. | None. | JSON: `{ "command": string }`; returns `"NONE"` when the queue is empty. |
| `POST` | `/api/mobile/notify` | Queue a mobile notification for the bot. | Query parameters: `app_name` (string, required) and `msg` (string, required). `app_name` is normalized to alphanumeric uppercase; `msg` is sanitized and limited to 34 characters. | JSON: `{ "status": "queued" }` |
| `POST` | `/api/mobile/action` | Apply a pet interaction and queue the corresponding bot action. | Query parameter: `action_type` (string, optional; default `pet`). Supported actions: `pizza`, `love`, `pet`, `sleep`. | JSON: `{ "status": "ok", "metrics": { "affection": number, "hunger": number, "energy": number, "pats": number, "feeds": number, "chats": number } }`. Unknown actions return `ok` without changing metrics. |
| `GET` | `/api/emo/poll_action` | Retrieve and remove the oldest pending bot action. | None. | JSON: `{ "action": string }`; returns `"NONE"` when the queue is empty. |
| `POST` | `/api/emo/clear_notifications` | Remove queued `NOTIF:` actions, used when Focus Mode starts. | None. | JSON: `{ "status": "cleared" }` |
| `POST` | `/api/emo/chat` | Convert a voice request into a spoken, emotion-tagged Divaan reply. | Raw request body containing signed 16-bit, 16 kHz, mono PCM. No JSON wrapper. Payloads shorter than 1,000 bytes are rejected. | Success: `200`, `Content-Type: application/octet-stream`, body containing signed 16-bit, 16 kHz, mono PCM. Headers: `X-Emo-Reply: string`, `X-Emo-Emotion: HAPPY\|SAD\|NEUTRAL`, and `Content-Length`. Short audio: `400` with `X-Emo-Reply: Audio Short`. Missing API key or processing failure: `500` with an explanatory `X-Emo-Reply` and `X-Emo-Emotion: SAD`. |

### Action and Command Queue Values

The queues are FIFO and held in memory. Typical values include:

- Media commands: client-provided values such as `PLAY`, `PAUSE`, or `NEXT`.
- Bot actions from media sync: `SONG:<title> - <artist>`.
- Bot actions from notifications: `NOTIF:[<APP>] <message>`.
- Pet interactions: `FEED_PIZZA` and `PET_LOVE`.

### Example Requests

```bash
# Read status
curl http://localhost:8000/api/emo/status

# Sync media metadata
curl -X POST http://localhost:8000/api/mobile/media_sync \
  -H "Content-Type: application/json" \
  -d '{"title":"Blue Train","artist":"John Coltrane","is_playing":true,"source":"player"}'

# Queue an app notification
curl -X POST "http://localhost:8000/api/mobile/notify?app_name=Messages&msg=New%20message"

# Send a pet action
curl -X POST "http://localhost:8000/api/mobile/action?action_type=pizza"

# Send raw PCM and save the returned audio
curl -X POST http://localhost:8000/api/emo/chat \
  -H "Content-Type: application/octet-stream" \
  --data-binary @input.pcm \
  -o reply.pcm
```

## Audio Processing Flow

`POST /api/emo/chat` follows this pipeline:

1. **PCM RAW ingestion**: Read the complete HTTP request body as raw bytes. The expected source format is signed 16-bit, 16 kHz, mono PCM.
2. **In-memory WAV wrapping**: Put those bytes into an in-memory WAV container with one channel, 2-byte samples, and a 16,000 Hz sample rate.
3. **Speech-to-text**: Pass the in-memory WAV to `SpeechRecognition`, which calls Google Speech Recognition. If recognition fails, the server uses the fallback text `Say hello to Aathi!`.
4. **Gemini multi-turn prompting**: Send the recognized text with the Divaan system prompt to Gemini. The request targets `gemini-2.5-flash` first and falls back to `gemini-2.0-flash` if needed.
5. **JSON structured extraction**: Request `application/json` output, then extract `reply` and `emotion` from the first Gemini candidate. The default is `Hey Aathi, I am ready!` with `HAPPY`.
6. **Google TTS generation**: Request the reply from the Google Translate TTS endpoint (`tl=en`).
7. **Miniaudio conversion**: Decode the returned audio to mono signed 16-bit PCM at 16 kHz. This is the output format consumed by the client.
8. **HTTP binary response**: Return the PCM bytes as `application/octet-stream`, with `X-Emo-Reply` and `X-Emo-Emotion` custom headers. Reply headers are sanitized to ASCII-safe values.

## Environment Configuration

Create `.env.local` in the project root, next to `requirements.txt` and `Procfile`:

```dotenv
# Required for /api/emo/chat
GEMINI_API_KEY=your_google_gemini_api_key

# Optional locally; Render supplies PORT automatically.
PORT=8000
```

### Configuration Keys

| Key | Required | Description |
|---|---|---|
| `GEMINI_API_KEY` | Yes for chat | Google Generative Language API key. The server reads it before calling Gemini. `/api/emo/chat` returns `500` with `X-Emo-Reply: No API Key` when it is absent. |
| `PORT` | No locally; yes as a platform convention on Render | Port for the Uvicorn process. Render sets this dynamically; do not hard-code it in the deployment command. |

The application loads `.env.local` itself at startup. Do not commit this file or place real credentials in documentation; `.env*.local` is ignored by Git in this repository. On Render, configure `GEMINI_API_KEY` in the service's Environment Variables instead of uploading `.env.local`. The `python-dotenv` package is present in `requirements.txt`, but the current loader uses the standard library directly.

## Deployment on Render

1. Push the repository to a Git provider and create a new **Web Service** in Render.
2. Select the repository and the branch to deploy.
3. Use the project root as the service root. The root must contain `requirements.txt`, `Procfile`, and the `api/` package directory.
4. Set the runtime to Python. Render installs dependencies from `requirements.txt` during the build.
5. Set the build command to:

   ```text
   pip install -r requirements.txt
   ```

6. Set the start command to:

   ```text
   uvicorn api.index:app --host 0.0.0.0 --port $PORT
   ```

   The repository `Procfile` contains the equivalent Web Service command:

   ```text
   web: uvicorn api.index:app --host 0.0.0.0 --port $PORT
   ```

7. Add `GEMINI_API_KEY` under **Environment > Environment Variables**. Keep the value secret and do not commit it.
8. Deploy, then verify the service with `GET https://<service-name>.onrender.com/api/emo/status`.

Uvicorn runs the FastAPI application as an ASGI worker. For a small single-instance service, the command above is sufficient. If using multiple workers, remember that in-memory media/action queues and metrics are isolated per worker; use one worker or move shared state to an external store before scaling horizontally.

## Local Development

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn api.index:app --reload --host 0.0.0.0 --port 8000
```

The dashboard health response is available at `http://localhost:8000/` and returns a small HTML status page.
