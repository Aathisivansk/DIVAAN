import os
import io
import json
import wave
import re
import urllib.parse
import traceback
import httpx
import miniaudio
import speech_recognition as sr
from fastapi import FastAPI, HTTPException, Query, Request, Response
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# Centralized Emotion & Device Synchronization State
pet_metrics = {
    "affection": 85,
    "hunger": 75,
    "energy": 90,
    "pats": 12,
    "feeds": 5,
    "chats": 8,
    "battery_bars": 3,
    "is_charging": False,
    "alarm_hour": 7,
    "alarm_min": 30,
    "alarm_enabled": True
}

current_media_state = {
    "title": "No Track Playing",
    "artist": "",
    "is_playing": False,
    "source": "STANDBY"
}

pending_bot_actions = []
pending_phone_commands = []

def sanitize_header_value(val: str) -> str:
    if not val: return "None"
    clean = re.sub(r'[\r\n\t]+', ' ', str(val))
    clean = clean.encode('ascii', 'ignore').decode('ascii').strip()
    return clean[:120] if clean else "OK"

@app.get("/", response_class=HTMLResponse)
def get_dashboard():
    return "<h1>Divaan Unified Core Online</h1>"

@app.get("/api/device/manifest.json")
async def get_ota_manifest():
    return JSONResponse({
        "version": "v1.10.0",
        "notes": "Fix in AI integrations and minor bug fixes.",
        "firmware_url": "https://divaan-backend.onrender.com/api/device/firmware.bin"
    })

@app.get("/api/device/firmware.bin")
async def serve_firmware():
    bin_path = os.path.join(BASE_DIR, "firmware.bin")
    if not os.path.exists(bin_path):
        bin_path = os.path.join(BASE_DIR, "..", "firmware.bin")
    if not os.path.exists(bin_path):
        return Response(content="firmware.bin not found on server", status_code=404)
    return FileResponse(bin_path, media_type="application/octet-stream", filename="firmware.bin")

@app.get("/api/emo/status")
def get_status():
    return JSONResponse(content={
        "status": "online",
        "affection": pet_metrics["affection"],
        "hunger": pet_metrics["hunger"],
        "energy": pet_metrics["energy"],
        "pats": pet_metrics["pats"],
        "feeds": pet_metrics["feeds"],
        "chats": pet_metrics["chats"],
        "battery_bars": pet_metrics["battery_bars"],
        "is_charging": pet_metrics["is_charging"],
        "alarm": {
            "hour": pet_metrics["alarm_hour"],
            "min": pet_metrics["alarm_min"],
            "enabled": pet_metrics["alarm_enabled"]
        },
        "media": current_media_state
    })

@app.post("/api/mobile/media_sync")
async def sync_media_from_phone(request: Request):
    try:
        data = await request.json()
        title = data.get("title", "No Track")[:36]
        artist = data.get("artist", "")[:28]
        is_playing = data.get("is_playing", False)
        source = data.get("source", "player")

        current_media_state.update({"title": title, "artist": artist, "is_playing": is_playing, "source": source})
        display_str = f"{title} - {artist}" if artist else title
        
        # Remove old queued songs to avoid backlog
        pending_bot_actions[:] = [act for act in pending_bot_actions if not (isinstance(act, str) and act.startswith("SONG:"))]
        pending_bot_actions.append(f"SONG:{display_str[:40]}")
        return {"status": "ok"}
    except Exception as e:
        return JSONResponse(status_code=400, content={"error": str(e)})

@app.post("/api/mobile/notify")
def push_notification(app_name: str = Query(...), msg: str = Query(...)):
    clean_app = re.sub(r'[^a-zA-Z0-9]', '', app_name).upper()
    clean_msg = re.sub(r'[^a-zA-Z0-9 !?.:\-\[\]]', '', msg)[:34]
    
    # Cap queue size to prevent flooding
    if len(pending_bot_actions) >= 6:
        pending_bot_actions.pop(0)
    pending_bot_actions.append(f"NOTIF:[{clean_app}] {clean_msg}")
    return {"status": "queued"}

@app.post("/api/mobile/action")
async def handle_mobile_action(action_type: str = Query(...)):
    act = action_type.lower().strip()
    if act == "pizza" or act == "feed":
        pet_metrics["hunger"] = min(100, pet_metrics["hunger"] + 25)
        pet_metrics["affection"] = min(100, pet_metrics["affection"] + 5)
        pet_metrics["feeds"] += 1
        pending_bot_actions.append("FEED_PIZZA")
    elif act == "love" or act == "pet":
        pet_metrics["affection"] = min(100, pet_metrics["affection"] + 10)
        pet_metrics["pats"] += 1
        pending_bot_actions.append("PET_LOVE")
    elif act == "sleep":
        pending_bot_actions.append("FORCE_SLEEP")
    elif act == "check_ota":
        pending_bot_actions.append("CHECK_OTA")

    return JSONResponse({
        "status": "ok",
        "metrics": pet_metrics
    })

@app.post("/api/mobile/set_alarm")
def set_alarm_from_mobile(hour: int = Query(...), minute: int = Query(...), enabled: bool = Query(True)):
    pet_metrics["alarm_hour"] = hour
    pet_metrics["alarm_min"] = minute
    pet_metrics["alarm_enabled"] = enabled
    pending_bot_actions.append(f"SET_ALARM:{hour}:{minute}:{1 if enabled else 0}")
    return {"status": "alarm_configured", "alarm": {"hour": hour, "min": minute, "enabled": enabled}}

@app.post("/api/bot/sync_telemetry")
async def sync_telemetry_from_bot(request: Request):
    try:
        data = await request.json()
        pet_metrics["battery_bars"] = data.get("bars", pet_metrics["battery_bars"])
        pet_metrics["is_charging"] = data.get("is_charging", pet_metrics["is_charging"])
        pet_metrics["affection"] = data.get("affection", pet_metrics["affection"])
        pet_metrics["hunger"] = data.get("hunger", pet_metrics["hunger"])
        pet_metrics["energy"] = data.get("energy", pet_metrics["energy"])
        return {"status": "telemetry_received"}
    except Exception as e:
        return JSONResponse(status_code=400, content={"error": str(e)})

@app.post("/api/bot/media_cmd")
def receive_cmd_from_divaan(cmd: str = Query(...)):
    clean_cmd = cmd.upper().strip()
    pending_phone_commands.append(clean_cmd)
    return {"status": "dispatched", "command": clean_cmd}

@app.get("/api/mobile/poll_media_cmd")
def poll_media_cmd_for_phone():
    if pending_phone_commands:
        return {"command": pending_phone_commands.pop(0)}
    return {"command": "NONE"}

@app.get("/api/emo/poll_action")
async def poll_action():
    if pending_bot_actions:
        act = pending_bot_actions.pop(0)
        return JSONResponse({"action": act})
    return JSONResponse({"action": "NONE"})

@app.post("/api/emo/chat")
async def process_audio(request: Request):
    audio_bytes = await request.body()
    if not audio_bytes or len(audio_bytes) < 1000:
        print(f"[AUDIO REJECTED] Payload too small: {len(audio_bytes) if audio_bytes else 0} bytes")
        return Response(content=b"", status_code=400, headers={"X-Emo-Reply": "Audio Short", "X-Emo-Emotion": "SAD"})
         
    curr_key = os.environ.get("GEMINI_API_KEY")
    if not curr_key:
        print("[ERROR] GEMINI_API_KEY not found in environment!")
        return Response(content=b"", status_code=500, headers={"X-Emo-Reply": "No API Key", "X-Emo-Emotion": "SAD"})

    try:
        # Wrap raw PCM into WAV format (16kHz, mono, 16-bit)
        wav_io = io.BytesIO()
        with wave.open(wav_io, 'wb') as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(16000)
            wav_file.writeframes(audio_bytes)
        
        wav_data = wav_io.getvalue()
        print(f"[AUDIO RECEIVED] Raw PCM size: {len(audio_bytes)} bytes | WAV size: {len(wav_data)} bytes")

        recognizer = sr.Recognizer()
        user_text = ""
        
        try:
            with io.BytesIO(wav_data) as af:
                with sr.AudioFile(af) as source:
                    # Let recognizer adjust for ambient noise dynamically
                    recognizer.adjust_for_ambient_noise(source, duration=0.2)
                    audio_data = recognizer.record(source)
                    user_text = recognizer.recognize_google(audio_data)
                    print(f"[STT SUCCESS] User said: '{user_text}'")
        except sr.UnknownValueError:
            print("[STT WARN] Google Speech could not understand audio (Noise/Clipping/Silence)")
            user_text = ""
        except sr.RequestError as e:
            print(f"[STT ERROR] Google Speech API request failed: {e}")
            user_text = ""
        except Exception as e:
            print(f"[STT UNEXPECTED ERROR] {e}")
            user_text = ""

        # If audio could not be recognized, reply with confusion rather than a greeting
        if not user_text:
            reply_text = "I didn't hear you clearly."
            emotion = "SAD"
        else:
            prompt = (
                "You are Divaan, a witty and cute robot companion. "
                "Reply naturally in 1 short spoken sentence (strictly under 7 words). "
                "Return JSON ONLY: {\"reply\": \"<text>\", \"emotion\": \"HAPPY\" | \"SAD\" | \"NEUTRAL\"}"
            )
            payload = {
                "contents": [{"parts": [{"text": f"{prompt}\nUser: {user_text}"}]}],
                "generationConfig": {"responseMimeType": "application/json", "maxOutputTokens": 40}
            }

            reply_text = "I'm listening!"
            emotion = "HAPPY"
            models = ["gemini-2.5-flash", "gemini-2.0-flash"]
            
            async with httpx.AsyncClient(timeout=8.0) as client:
                for m in models:
                    try:
                        url = f"https://generativelanguage.googleapis.com/v1beta/models/{m}:generateContent?key={curr_key}"
                        resp = await client.post(url, json=payload)
                        if resp.status_code == 200:
                            data = resp.json()
                            res_json = json.loads(data["candidates"][0]["content"]["parts"][0]["text"])
                            reply_text = res_json.get("reply", "I hear you!")
                            emotion = res_json.get("emotion", "HAPPY")
                            print(f"[LLM SUCCESS] Model: {m} | Reply: '{reply_text}' | Emotion: {emotion}")
                            break
                    except Exception as llm_err:
                        print(f"[LLM WARN] Failed with model {m}: {llm_err}")

        # Text to Speech synthesis via Google TTS
        encoded = urllib.parse.quote(reply_text)
        tts_url = f"https://translate.google.com/translate_tts?ie=UTF-8&q={encoded}&tl=en&client=tw-ob"
        
        pcm_out = b""
        async with httpx.AsyncClient(timeout=6.0) as client:
            tts_res = await client.get(tts_url, headers={"User-Agent": "Mozilla/5.0"})
            if tts_res.status_code == 200:
                decoded = miniaudio.decode(tts_res.content, nchannels=1, sample_rate=16000, output_format=miniaudio.SampleFormat.SIGNED16)
                pcm_out = bytes(decoded.samples)

        pet_metrics["chats"] += 1
        pet_metrics["affection"] = min(100, pet_metrics["affection"] + 2)

        headers = {
            "X-Emo-Reply": sanitize_header_value(reply_text),
            "X-Emo-Emotion": emotion,
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(pcm_out))
        }
        return Response(content=pcm_out, media_type="application/octet-stream", headers=headers, status_code=200)

    except Exception as e:
        traceback.print_exc()
        return Response(content=b"", status_code=500, headers={"X-Emo-Reply": "Server Error", "X-Emo-Emotion": "SAD"})

    
    audio_bytes = await request.body()
    if not audio_bytes or len(audio_bytes) < 1000:
        return Response(content=b"", status_code=400, headers={"X-Emo-Reply": "Audio Short", "X-Emo-Emotion": "SAD"})
    
    curr_key = os.environ.get("GEMINI_API_KEY")
    if not curr_key:
        return Response(content=b"", status_code=500, headers={"X-Emo-Reply": "No API Key", "X-Emo-Emotion": "SAD"})

    try:
        recognizer = sr.Recognizer()
        wav_io = io.BytesIO()
        with wave.open(wav_io, 'wb') as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(16000)
            wav_file.writeframes(audio_bytes)

        user_text = ""
        try:
            with io.BytesIO(wav_io.getvalue()) as af:
                with sr.AudioFile(af) as source:
                    recognizer.energy_threshold = 120
                    recognizer.dynamic_energy_threshold = False
                    audio_data = recognizer.record(source)
                    user_text = recognizer.recognize_google(audio_data)
        except Exception:
            user_text = "Say hello to Aathi!"

        prompt = (
            "You are Divaan, a witty robot pet. "
            "Reply naturally in 1 short sentence (under 6 words). "
            "Return JSON: {\"reply\": \"<text>\", \"emotion\": \"HAPPY\" | \"SAD\" | \"NEUTRAL\"}"
        )
        payload = {
            "contents": [{"parts": [{"text": f"{prompt}\nUser: {user_text}"}]}],
            "generationConfig": {"responseMimeType": "application/json", "maxOutputTokens": 40}
        }

        reply_text = "Hey Aathi, I am ready!"
        emotion = "HAPPY"
        models = ["gemini-2.5-flash", "gemini-2.0-flash"]
        async with httpx.AsyncClient(timeout=8.0) as client:
            for m in models:
                try:
                    url = f"https://generativelanguage.googleapis.com/v1beta/models/{m}:generateContent?key={curr_key}"
                    resp = await client.post(url, json=payload)
                    if resp.status_code == 200:
                        data = resp.json()
                        res_json = json.loads(data["candidates"][0]["content"]["parts"][0]["text"])
                        reply_text = res_json.get("reply", "Hey! Ready!")
                        emotion = res_json.get("emotion", "HAPPY")
                        break
                except Exception:
                    pass

        encoded = urllib.parse.quote(reply_text)
        tts_url = f"https://translate.google.com/translate_tts?ie=UTF-8&q={encoded}&tl=en&client=tw-ob"
        async with httpx.AsyncClient(timeout=6.0) as client:
            tts_res = await client.get(tts_url, headers={"User-Agent": "Mozilla/5.0"})
            decoded = miniaudio.decode(tts_res.content, nchannels=1, sample_rate=16000, output_format=miniaudio.SampleFormat.SIGNED16)
            pcm_out = bytes(decoded.samples)

        pet_metrics["chats"] += 1
        pet_metrics["affection"] = min(100, pet_metrics["affection"] + 2)

        headers = {
            "X-Emo-Reply": sanitize_header_value(reply_text),
            "X-Emo-Emotion": emotion,
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(pcm_out))
        }
        return Response(content=pcm_out, media_type="application/octet-stream", headers=headers, status_code=200)
    except Exception as e:
        traceback.print_exc()
        return Response(content=b"", status_code=500, headers={"X-Emo-Reply": "Error", "X-Emo-Emotion": "SAD"})