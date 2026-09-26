import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const DivaanApp());
}

class DivaanTheme {
  static const Color cyanPrimary = Color(0xFF00E5FF);
  static const Color cyanGlow = Color(0xFF18FFFF);
  static const Color darkBg = Color(0xFF0A0E14);
  static const Color darkCard = Color(0xFF121824);
  static const Color darkBorder = Color(0xFF1E293B);
  static const Color lightBg = Color(0xFFF8FAFC);
  static const Color lightCard = Color(0xFFFFFFFF);

  static ThemeData darkTheme = ThemeData(
    brightness: Brightness.dark,
    scaffoldBackgroundColor: darkBg,
    cardColor: darkCard,
    colorScheme: const ColorScheme.dark(primary: cyanPrimary, secondary: cyanGlow, surface: darkCard),
    textTheme: GoogleFonts.spaceGroteskTextTheme(ThemeData.dark().textTheme),
    appBarTheme: const AppBarTheme(backgroundColor: darkBg, elevation: 0, centerTitle: true),
  );

  static ThemeData lightTheme = ThemeData(
    brightness: Brightness.light,
    scaffoldBackgroundColor: lightBg,
    cardColor: lightCard,
    colorScheme: const ColorScheme.light(primary: Color(0xFF00838F), secondary: cyanPrimary, surface: lightCard),
    textTheme: GoogleFonts.spaceGroteskTextTheme(ThemeData.light().textTheme),
    appBarTheme: const AppBarTheme(
      backgroundColor: lightBg,
      elevation: 0,
      centerTitle: true,
      iconTheme: IconThemeData(color: Colors.black),
      titleTextStyle: TextStyle(color: Colors.black, fontSize: 18, fontWeight: FontWeight.bold),
    ),
  );
}

class DivaanState extends ChangeNotifier {
  static const MethodChannel _nativeMethod = MethodChannel('com.divaan.companion/native_bridge');
  static const EventChannel _nativeEvent = EventChannel('com.divaan.companion/notification_stream');
  static const String permanentBaseUrl = "https://divaan-backend.onrender.com";

  bool isDarkMode = true;
  bool isConnected = false;
  bool isConnecting = false;
  bool isPermissionGranted = false;

  int affection = 85;
  int hunger = 75;
  int energy = 90;
  int pats = 0;
  int feeds = 0;
  int chats = 0;
  int batteryBars = 3;
  bool isCharging = false;

  int alarmHour = 7;
  int alarmMin = 30;
  bool alarmEnabled = true;

  String currentTrack = "No Track Playing";
  String currentArtist = "";
  String currentAppSource = "STANDBY";
  bool isMediaPlaying = false;
  String lastTrackDispatched = "";

  StreamSubscription? _streamSub;
  Timer? _healthTimer;
  Timer? _mediaPollTimer;

  DivaanState() {
    _init();
  }

  Future<void> _init() async {
    final prefs = await SharedPreferences.getInstance();
    isDarkMode = prefs.getBool("dark_mode") ?? true;
    notifyListeners();
    await checkPermissionStatus();
    _startContinuousLoops();
    _listenToNativeEvents();
    checkConnection();
  }

  void toggleTheme() async {
    isDarkMode = !isDarkMode;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool("dark_mode", isDarkMode);
    notifyListeners();
  }

  Future<void> checkPermissionStatus() async {
    try {
      final bool granted = await _nativeMethod.invokeMethod('isPermissionGranted');
      isPermissionGranted = granted;
      notifyListeners();
    } catch (_) {}
  }

  Future<void> requestNotificationAccess() async {
    try {
      await _nativeMethod.invokeMethod('openNotificationSettings');
    } catch (_) {}
  }

  Future<void> checkConnection() async {
    if (isConnecting) return;
    isConnecting = true;
    notifyListeners();
    try {
      final res = await http.get(
        Uri.parse("$permanentBaseUrl/api/emo/status"),
        headers: {"Accept": "application/json"},
      ).timeout(const Duration(seconds: 10));
      if (res.statusCode == 200) {
        isConnected = true;
        final data = jsonDecode(res.body);
        affection = data["affection"] ?? affection;
        hunger = data["hunger"] ?? hunger;
        energy = data["energy"] ?? energy;
        pats = data["pats"] ?? pats;
        feeds = data["feeds"] ?? feeds;
        chats = data["chats"] ?? chats;
        batteryBars = data["battery_bars"] ?? batteryBars;
        isCharging = data["is_charging"] ?? isCharging;

        final alm = data["alarm"] ?? {};
        alarmHour = alm["hour"] ?? alarmHour;
        alarmMin = alm["min"] ?? alarmMin;
        alarmEnabled = alm["enabled"] ?? alarmEnabled;
      } else {
        isConnected = false;
      }
    } catch (_) {
      isConnected = false;
    } finally {
      isConnecting = false;
      notifyListeners();
    }
  }

  void _startContinuousLoops() {
    _healthTimer?.cancel();
    _healthTimer = Timer.periodic(const Duration(seconds: 4), (_) {
      checkConnection();
      checkPermissionStatus();
    });

    _mediaPollTimer?.cancel();
    _mediaPollTimer = Timer.periodic(const Duration(milliseconds: 1000), (_) => _pollBotMediaCommands());
  }

  Future<void> _pollBotMediaCommands() async {
    if (!isConnected) return;
    try {
      final res = await http.get(Uri.parse("$permanentBaseUrl/api/mobile/poll_media_cmd")).timeout(const Duration(seconds: 2));
      if (res.statusCode == 200) {
        final data = jsonDecode(res.body);
        final cmd = data["command"];
        if (cmd == "PLAY_PAUSE") sendMediaAction("play_pause");
        else if (cmd == "NEXT") sendMediaAction("next");
        else if (cmd == "PREV") sendMediaAction("previous");
      }
    } catch (_) {}
  }

  Future<void> sendMediaAction(String action) async {
    try {
      await _nativeMethod.invokeMethod('media_control', {'action': action});
      if (action == "play_pause") isMediaPlaying = !isMediaPlaying;
      notifyListeners();
    } catch (_) {}
  }

  void _listenToNativeEvents() {
    _streamSub?.cancel();
    _streamSub = _nativeEvent.receiveBroadcastStream().listen((dynamic event) {
      if (event == null || event is! Map) return;
      final pkg = (event['package'] ?? '').toString().toLowerCase();
      final title = (event['title'] ?? '').toString();
      final text = (event['text'] ?? '').toString();

      if (pkg.contains("spotify") || pkg.contains("youtube") || pkg.contains("music") || pkg.contains("audio") || pkg.contains("player")) {
        String source = "SPOTIFY";
        if (pkg.contains("youtube.music")) source = "YT MUSIC";
        else if (pkg.contains("youtube")) source = "YOUTUBE";
        _syncMedia(title, text, source, true);
      } else if (pkg.contains("whatsapp") || pkg.contains("instagram")) {
        String appName = pkg.contains("whatsapp") ? "WHATSAPP" : "INSTAGRAM";
        http.post(Uri.parse("$permanentBaseUrl/api/mobile/notify?app_name=$appName&msg=${Uri.encodeComponent('$title: $text')}"));
      }
    });
  }

  Future<void> _syncMedia(String title, String artist, String source, bool isPlaying) async {
    currentTrack = title.isNotEmpty ? title : "Playing Audio";
    currentArtist = artist;
    currentAppSource = source;
    isMediaPlaying = isPlaying;
    notifyListeners();

    final sig = "$title|$artist|$isPlaying";
    if (sig == lastTrackDispatched) return;
    lastTrackDispatched = sig;

    try {
      await http.post(
        Uri.parse("$permanentBaseUrl/api/mobile/media_sync"),
        headers: {"Content-Type": "application/json"},
        body: jsonEncode({
          "title": title,
          "artist": artist,
          "source": source,
          "is_playing": isPlaying,
        }),
      ).timeout(const Duration(seconds: 4));
    } catch (_) {}
  }

  Future<void> sendBotAction(String actionType) async {
    try {
      final res = await http.post(
        Uri.parse("$permanentBaseUrl/api/mobile/action?action_type=$actionType"),
        headers: {"Accept": "application/json"},
      );
      if (res.statusCode == 200) {
        final data = jsonDecode(res.body);
        final metrics = data["metrics"] ?? {};
        affection = metrics["affection"] ?? affection;
        hunger = metrics["hunger"] ?? hunger;
        energy = metrics["energy"] ?? energy;
        pats = metrics["pats"] ?? pats;
        feeds = metrics["feeds"] ?? feeds;
        notifyListeners();
      }
    } catch (_) {}
  }

  Future<void> syncAlarm(int hour, int min, bool enabled) async {
    alarmHour = hour;
    alarmMin = min;
    alarmEnabled = enabled;
    notifyListeners();
    try {
      await http.post(
        Uri.parse("$permanentBaseUrl/api/mobile/set_alarm?hour=$hour&minute=$min&enabled=$enabled"),
      );
    } catch (_) {}
  }

  @override
  void dispose() {
    _healthTimer?.cancel();
    _mediaPollTimer?.cancel();
    _streamSub?.cancel();
    super.dispose();
  }
}

class DivaanApp extends StatefulWidget {
  const DivaanApp({super.key});

  @override
  State<DivaanApp> createState() => _DivaanAppState();
}

class _DivaanAppState extends State<DivaanApp> {
  final DivaanState _state = DivaanState();

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _state,
      builder: (context, _) {
        return MaterialApp(
          title: 'Divaan Companion',
          debugShowCheckedModeBanner: false,
          theme: DivaanTheme.lightTheme,
          darkTheme: DivaanTheme.darkTheme,
          themeMode: _state.isDarkMode ? ThemeMode.dark : ThemeMode.light,
          home: HomeScreen(state: _state),
        );
      },
    );
  }
}

class HomeScreen extends StatelessWidget {
  final DivaanState state;
  const HomeScreen({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    final isDark = state.isDarkMode;
    final primary = isDark ? DivaanTheme.cyanPrimary : const Color(0xFF00838F);

    return Scaffold(
      appBar: AppBar(
        title: Text(
          "DIVAAN ECOSYSTEM",
          style: GoogleFonts.orbitron(letterSpacing: 1.5, fontWeight: FontWeight.bold, fontSize: 16),
        ),
        actions: [
          IconButton(icon: Icon(isDark ? Icons.light_mode : Icons.dark_mode), onPressed: state.toggleTheme),
        ],
      ),
      body: RefreshIndicator(
        onRefresh: state.checkConnection,
        child: SingleChildScrollView(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _buildConnectionHeader(context, primary),
              const SizedBox(height: 14),
              _buildLiveMediaCenter(context, primary),
              const SizedBox(height: 14),
              _buildMetricsCard(context, primary),
              const SizedBox(height: 14),
              _buildAlarmCard(context, primary),
              const SizedBox(height: 14),
              _buildActionGrid(context, primary),
              const SizedBox(height: 14),
              _buildListenerStatus(context, primary),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildConnectionHeader(BuildContext context, Color primary) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Theme.of(context).cardColor,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: state.isConnected ? primary : Colors.redAccent, width: 1.2),
      ),
      child: Row(
        children: [
          Icon(
            state.isConnected ? Icons.cloud_done_rounded : (state.isConnecting ? Icons.cloud_sync_rounded : Icons.cloud_off_rounded),
            color: state.isConnected ? primary : (state.isConnecting ? Colors.amber : Colors.redAccent),
            size: 28,
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  state.isConnected ? "CONNECTED & SYNCED" : (state.isConnecting ? "CONNECTING..." : "DISCONNECTED"),
                  style: GoogleFonts.orbitron(
                    color: state.isConnected ? primary : (state.isConnecting ? Colors.amber : Colors.redAccent),
                    fontWeight: FontWeight.bold,
                    fontSize: 13,
                  ),
                ),
                Text(DivaanState.permanentBaseUrl, style: const TextStyle(fontSize: 10, color: Colors.grey), overflow: TextOverflow.ellipsis),
              ],
            ),
          ),
          IconButton(
            icon: state.isConnecting
                ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.refresh),
            onPressed: state.checkConnection,
          ),
        ],
      ),
    );
  }

  Widget _buildLiveMediaCenter(BuildContext context, Color primary) {
    return Container(
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        color: Theme.of(context).cardColor,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: primary.withOpacity(0.3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text("ACTIVE MEDIA CONTROLLER", style: GoogleFonts.orbitron(fontSize: 11, fontWeight: FontWeight.bold)),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(color: primary.withOpacity(0.15), borderRadius: BorderRadius.circular(6)),
                child: Text(
                  state.currentAppSource,
                  style: TextStyle(color: primary, fontSize: 9, fontWeight: FontWeight.bold),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Text(state.currentTrack, style: const TextStyle(fontSize: 15, fontWeight: FontWeight.bold), maxLines: 1, overflow: TextOverflow.ellipsis),
          if (state.currentArtist.isNotEmpty)
            Text(state.currentArtist, style: const TextStyle(fontSize: 12, color: Colors.grey)),
          const SizedBox(height: 16),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            children: [
              IconButton(
                iconSize: 32,
                icon: const Icon(Icons.skip_previous_rounded),
                onPressed: () => state.sendMediaAction("previous"),
              ),
              IconButton(
                iconSize: 42,
                color: primary,
                icon: Icon(state.isMediaPlaying ? Icons.pause_circle_filled_rounded : Icons.play_circle_fill_rounded),
                onPressed: () => state.sendMediaAction("play_pause"),
              ),
              IconButton(
                iconSize: 32,
                icon: const Icon(Icons.skip_next_rounded),
                onPressed: () => state.sendMediaAction("next"),
              ),
            ],
          )
        ],
      ),
    );
  }

  Widget _buildMetricsCard(BuildContext context, Color primary) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(color: Theme.of(context).cardColor, borderRadius: BorderRadius.circular(14)),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text("EMOTION & RELATIONSHIP", style: GoogleFonts.orbitron(fontSize: 11, fontWeight: FontWeight.bold)),
              Text("${state.pats} Pats • ${state.feeds} Feeds", style: const TextStyle(fontSize: 10, color: Colors.grey)),
            ],
          ),
          const SizedBox(height: 12),
          _metricRow("Affection", state.affection, const Color(0xFFFF4081)),
          const SizedBox(height: 8),
          _metricRow("Hunger", state.hunger, const Color(0xFFFFAB00)),
          const SizedBox(height: 8),
          _metricRow("Energy", state.energy, const Color(0xFF00E676)),
        ],
      ),
    );
  }

  Widget _metricRow(String label, int val, Color color) {
    return Row(
      children: [
        SizedBox(width: 75, child: Text(label, style: const TextStyle(fontSize: 11))),
        Expanded(
          child: ClipRRect(
            borderRadius: BorderRadius.circular(4),
            child: LinearProgressIndicator(value: val / 100.0, color: color, backgroundColor: color.withOpacity(0.15), minHeight: 6),
          ),
        ),
        const SizedBox(width: 10),
        Text("$val%", style: TextStyle(fontSize: 11, color: color, fontWeight: FontWeight.bold)),
      ],
    );
  }

  Widget _buildAlarmCard(BuildContext context, Color primary) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(color: Theme.of(context).cardColor, borderRadius: BorderRadius.circular(14)),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Row(
            children: [
              Icon(Icons.alarm_rounded, color: state.alarmEnabled ? primary : Colors.grey, size: 28),
              const SizedBox(width: 12),
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text("DIVAAN ALARM", style: GoogleFonts.orbitron(fontSize: 11, fontWeight: FontWeight.bold)),
                  Text(
                    "${state.alarmHour.toString().padLeft(2, '0')}:${state.alarmMin.toString().padLeft(2, '0')}",
                    style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: state.alarmEnabled ? Colors.white : Colors.grey),
                  ),
                ],
              ),
            ],
          ),
          Row(
            children: [
              IconButton(
                icon: const Icon(Icons.edit_calendar_rounded),
                onPressed: () async {
                  final time = await showTimePicker(
                    context: context,
                    initialTime: TimeOfDay(hour: state.alarmHour, minute: state.alarmMin),
                  );
                  if (time != null) {
                    state.syncAlarm(time.hour, time.minute, state.alarmEnabled);
                  }
                },
              ),
              Switch(
                value: state.alarmEnabled,
                activeColor: primary,
                onChanged: (val) => state.syncAlarm(state.alarmHour, state.alarmMin, val),
              ),
            ],
          )
        ],
      ),
    );
  }

  Widget _buildActionGrid(BuildContext context, Color primary) {
    return Row(
      children: [
        Expanded(
          child: ElevatedButton.icon(
            style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFFFF9100), foregroundColor: Colors.black),
            icon: const Icon(Icons.local_pizza_rounded, size: 16),
            label: const Text("Feed Pizza", style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
            onPressed: () => state.sendBotAction("pizza"),
          ),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: ElevatedButton.icon(
            style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFFFF1744), foregroundColor: Colors.white),
            icon: const Icon(Icons.favorite_rounded, size: 16),
            label: const Text("Send Love", style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
            onPressed: () => state.sendBotAction("love"),
          ),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: ElevatedButton.icon(
            style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFF651FFF), foregroundColor: Colors.white),
            icon: const Icon(Icons.bedtime_rounded, size: 16),
            label: const Text("Sleep", style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
            onPressed: () => state.sendBotAction("sleep"),
          ),
        ),
      ],
    );
  }

  Widget _buildListenerStatus(BuildContext context, Color primary) {
    return OutlinedButton.icon(
      style: OutlinedButton.styleFrom(
        side: BorderSide(color: state.isPermissionGranted ? Colors.green : primary),
        padding: const EdgeInsets.symmetric(vertical: 12),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      ),
      icon: Icon(state.isPermissionGranted ? Icons.check_circle_rounded : Icons.sensors_rounded, color: state.isPermissionGranted ? Colors.green : primary),
      label: Text(
        state.isPermissionGranted ? "Divaan Companion Active" : "Grant Notification & Media Access",
        style: TextStyle(color: state.isPermissionGranted ? Colors.green : primary, fontSize: 12),
      ),
      onPressed: state.requestNotificationAccess,
    );
  }
}