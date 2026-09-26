# Divaan Companion Flutter Application

Divaan Companion is the mobile companion for the Divaan ecosystem. It presents the Flutter UI, synchronizes companion state with the backend, observes Android media notifications, and dispatches remote media controls.

## Architecture Overview

The application has two cooperating layers:

- **Flutter UI layer**: `lib/main.dart` owns the screens, backend polling, media synchronization, and the single application state object, `DivaanState`.
- **Native Android layer**: `android/app/src/main/kotlin/com/example/divaan/MainActivity.kt` exposes Android-only capabilities to Flutter through platform channels.

The channel contract is:

| Channel | Direction | Purpose |
| --- | --- | --- |
| `com.divaan.companion/native_bridge` (`MethodChannel`) | Flutter -> Android | Check notification access, open notification settings, and dispatch media actions. |
| `com.divaan.companion/notification_stream` (`EventChannel`) | Android -> Flutter | Stream notification payloads containing the source package, title, and text. |

Flutter invokes methods such as `isPermissionGranted`, `openNotificationSettings`, and `media_control`. The native listener publishes event maps with `package`, `title`, and `text`; `DivaanState` interprets media and messaging sources and updates the UI or backend accordingly.

## Native OS Integration

### Notification listener and background media interception

`DivaanNotificationListener` extends Android's `NotificationListenerService`. Android invokes `onNotificationPosted` when an enabled notification listener receives a notification, including while no Flutter screen is visible. The service:

1. Reads `android.title` and `android.text` from the notification extras.
2. Identifies likely media packages such as Spotify, YouTube, music players, and audio players.
3. Treats the title as the track title and the notification text as the artist or supporting track metadata.
4. Sends the media title, artist, source, and playback state to the Divaan backend on a worker thread.
5. Publishes the notification payload through `notification_stream` when Flutter is listening.

This OS-level background session interceptor does not depend on a Flutter widget or an active UI route. The native service currently reports posted media notifications as `is_playing: true`; Flutter also maintains its local playback state and can synchronize it with the backend.

### Remote media button dispatcher

Flutter calls the `media_control` method with `play_pause`, `next`, or `previous`. `MainActivity.performMediaAction` maps the action to an Android `KeyEvent` and uses `AudioManager.dispatchMediaKeyEvent` to send matching key-down and key-up events:

- `play_pause` -> `KEYCODE_MEDIA_PLAY_PAUSE`
- `next` -> `KEYCODE_MEDIA_NEXT`
- `previous` -> `KEYCODE_MEDIA_PREVIOUS`

This lets the companion control the active Android media session without embedding a player in the Flutter UI.

## Android Manifest Permissions

The Android application must declare these permissions:

```xml
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.BIND_NOTIFICATION_LISTENER_SERVICE" />
<uses-permission android:name="android.permission.MEDIA_CONTENT_CONTROL" />
```

The notification listener service must also be registered with the binding permission and the notification-listener intent action:

```xml
<service
	android:name=".DivaanNotificationListener"
	android:permission="android.permission.BIND_NOTIFICATION_LISTENER_SERVICE"
	android:exported="true">
	<intent-filter>
		<action android:name="android.service.notification.NotificationListenerService" />
	</intent-filter>
</service>
```

`BIND_NOTIFICATION_LISTENER_SERVICE` is enforced by Android for the service declaration. The user must additionally enable Divaan Companion in Android's notification access settings; the Flutter app checks that status through `isPermissionGranted` and can open the settings screen through `openNotificationSettings`.

## State Management and Persistence

`DivaanState` is the single source of truth for application state. It extends `ChangeNotifier`, so widgets rebuild reactively through `AnimatedBuilder` when connection status, companion metrics, permission state, theme, or media metadata changes.

Responsibilities include:

- polling backend health and companion metrics;
- receiving native notification events;
- synchronizing track title, artist, source, and playback state;
- dispatching remote media and companion actions; and
- exposing notification access and connection status to the UI.

Persistent preferences use the `shared_preferences` package. The current implementation stores the `dark_mode` preference and restores it when `DivaanState` initializes. New persisted fields should be read during initialization and written through the same state owner so widgets do not maintain competing copies of application state.

## Build and Sideload

### Build a release APK

From the repository root:

```powershell
flutter build apk --release
```

The generated APK is:

```text
build/app/outputs/flutter-apk/app-release.apk
```

### Install with ADB

With a USB-connected or ADB-visible Android device, install or update the release APK with:

```powershell
adb install -r -d build/app/outputs/flutter-apk/app-release.apk
```

`-r` reinstalls while retaining app data. `-d` permits installation when the APK version is lower than the version currently installed on the device, which is useful during local development.

After installation, open Android **Settings -> Notification access**, enable **Divaan Companion**, and return to the app. Without this user-granted access, Android will not deliver notification callbacks to `DivaanNotificationListener`.

### Google Play Protect warning

A locally built or sideloaded APK is not signed and distributed through Google Play. Google Play Protect may therefore label it as an unverified or unknown app. Verify that the APK came from your local build, then choose the available **Install anyway** or equivalent confirmation in the Play Protect prompt. This warning is separate from notification access: the app still needs the user to enable its notification listener in Android settings after installation.

## Project Locations

- Flutter application and state: `lib/main.dart`
- Android channel handlers and notification service: `android/app/src/main/kotlin/com/example/divaan/MainActivity.kt`
- Android manifest and service registration: `android/app/src/main/AndroidManifest.xml`
- Flutter dependencies: `pubspec.yaml`
