package com.divaan.companion

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.os.Build
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.view.KeyEvent
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONObject
import kotlin.concurrent.thread

class MainActivity : FlutterActivity() {
    private val METHOD_CHANNEL = "com.divaan.companion/native_bridge"
    private val EVENT_CHANNEL = "com.divaan.companion/notification_stream"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, METHOD_CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "isPermissionGranted" -> {
                    val enabledListeners = Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
                    result.success(enabledListeners != null && enabledListeners.contains(packageName))
                }
                "openNotificationSettings" -> {
                    val intent = Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    startActivity(intent)
                    result.success(true)
                }
                "media_control" -> {
                    val action = call.argument<String>("action") ?: ""
                    result.success(performMediaAction(this, action))
                }
                else -> result.notImplemented()
            }
        }

        EventChannel(flutterEngine.dartExecutor.binaryMessenger, EVENT_CHANNEL).setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    DivaanNotificationListener.eventSink = events
                }
                override fun onCancel(arguments: Any?) {
                    DivaanNotificationListener.eventSink = null
                }
            }
        )
    }

    companion object {
        fun performMediaAction(context: Context, action: String): Boolean {
            try {
                val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                val keyCode = when (action) {
                    "play_pause" -> KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE
                    "next" -> KeyEvent.KEYCODE_MEDIA_NEXT
                    "previous" -> KeyEvent.KEYCODE_MEDIA_PREVIOUS
                    else -> return false
                }
                audioManager.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keyCode))
                audioManager.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keyCode))
                return true
            } catch (_: Exception) {
                return false
            }
        }
    }
}

class DivaanNotificationListener : NotificationListenerService() {
    companion object {
        var eventSink: EventChannel.EventSink? = null
        private var lastDispatchedSig = ""
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn == null) return
        try {
            val extras = sbn.notification.extras ?: return
            val pkg = (sbn.packageName ?: "").lowercase()
            val title = extras.getCharSequence("android.title")?.toString() ?: ""
            val text = extras.getCharSequence("android.text")?.toString() ?: ""

            if (title.isEmpty() && text.isEmpty()) return

            val isMedia = pkg.contains("spotify") || pkg.contains("youtube") || pkg.contains("music") || pkg.contains("audio") || pkg.contains("player")

            if (isMedia) {
                var source = "SPOTIFY"
                if (pkg.contains("youtube.music")) source = "YT MUSIC"
                else if (pkg.contains("youtube")) source = "YOUTUBE"

                val mediaSig = "$title|$text"
                if (mediaSig != lastDispatchedSig) {
                    lastDispatchedSig = mediaSig

                    thread {
                        try {
                            val url = URL("https://divaan-backend.onrender.com/api/mobile/media_sync")
                            val conn = url.openConnection() as HttpURLConnection
                            conn.requestMethod = "POST"
                            conn.setRequestProperty("Content-Type", "application/json; utf-8")
                            conn.doOutput = true
                            conn.connectTimeout = 4000

                            val json = JSONObject().apply {
                                put("title", title)
                                put("artist", text)
                                put("source", source)
                                put("is_playing", true)
                            }

                            OutputStreamWriter(conn.outputStream).use { it.write(json.toString()) }
                            conn.responseCode
                            conn.disconnect()
                        } catch (_: Exception) {}
                    }
                }
            }

            android.os.Handler(android.os.Looper.getMainLooper()).post {
                try {
                    val payload = mapOf("package" to pkg, "title" to title, "text" to text)
                    eventSink?.success(payload)
                } catch (_: Exception) {}
            }
        } catch (_: Exception) {}
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {}
}