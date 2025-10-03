#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// WiFi Configuration
const char* ssid = "TimecodeMaster";
const char* password = "timecode123";

WiFiUDP udp;
Preferences preferences;

// Network configuration
const unsigned int UDP_PORT = 12345;
IPAddress masterIP(192, 168, 4, 1); // Master AP IP

// LTC SMPTE Audio Configuration
#define SAMPLE_RATE     48000
#define OUTPUT_PIN      25   // DAC1 (GPIO25) - LTC Audio Output
#define BITS_PER_FRAME  80

// LTC variables
uint8_t frameBits[BITS_PER_FRAME];
int bitIndex = 0;
int sampleIndex = 0;
bool currentLevel = false;
bool ltcEnabled = true;
int samplesPerBit = 0;

hw_timer_t *ltcTimer = NULL;
portMUX_TYPE ltcTimerMux = portMUX_INITIALIZER_UNLOCKED;

// Sync mode configuration
enum SyncMode {
  SYNC_MODE_HARD,    // Always hard sync
  SYNC_MODE_SOFT,    // Always soft sync  
  SYNC_MODE_AUTO     // Auto: soft sync if < 1s drift, hard sync if >= 1s
};

// Enhanced Timecode structure with soft sync
struct Timecode {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
  uint8_t frames;
  uint8_t fps;
  bool dropFrame;
  bool running;
  bool synced;
  bool connectedToMaster;
  int32_t driftFrames;
  unsigned long lastFrameMicros;
  unsigned long masterTimeOffset;
  uint32_t lastSequence;
  float driftCompensation;
  unsigned long lastPreciseUpdate;
  bool softSyncing;
  float syncRateAdjustment;
  unsigned long syncStartTime;
  uint32_t syncStartFrames;
  uint32_t targetFrames;
  SyncMode syncMode;
  unsigned long lastFrameAdvance; // Track when frames actually advance
};

Timecode currentTimecode = {0, 0, 0, 0, 25, false, true, false, false, 0, 0, 0, 0, 0.0, 0, false, 0.0, 0, 0, 0, SYNC_MODE_AUTO, 0};
Timecode lastMasterTimecode = {0, 0, 0, 0, 25, false, false, false, false, 0, 0, 0, 0, 0.0, 0};

// Client identification
String clientId;
String clientName = "Camera_01";

// Physical pins
const int SYNC_LED_PIN = 2;
const int STATUS_LED_PIN = 4;

// Enhanced timing constants
const unsigned long REGISTRATION_INTERVAL = 10000;
const unsigned long HEARTBEAT_INTERVAL = 2000;
const unsigned long STATUS_UPDATE_INTERVAL = 2000;
const unsigned long SYNC_TIMEOUT = 5000;

// Soft sync configuration - MORE AGGRESSIVE VALUES
const float MAX_SYNC_ADJUSTMENT = 0.2f;    // 20% max speed adjustment
const float MIN_SYNC_ADJUSTMENT = 0.01f;   // 1% min adjustment
const unsigned long SOFT_SYNC_TIMEOUT = 15000; // 15 seconds max for soft sync
const int SOFT_SYNC_THRESHOLD = 1;         // Start soft sync if drift > 1 frame

// Auto sync thresholds (in seconds) - CHANGED TO 1 SECOND
const float AUTO_SYNC_HARD_THRESHOLD = 1.0f; // 1 second threshold for hard sync in AUTO mode

// State variables
unsigned long lastRegistration = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastMasterPacket = 0;
unsigned long lastSerialOutput = 0;
unsigned long lastTimecodeUpdate = 0;

// Connection quality monitoring
struct ConnectionStats {
  uint32_t packetsSent;
  uint32_t packetsReceived;
  uint32_t syncRequests;
  unsigned long lastReconnection;
};

ConnectionStats connectionStats = {0, 0, 0, 0};

// Enhanced audio output with better levels
inline void dacWriteLevel(bool level) {
  dacWrite(OUTPUT_PIN, level ? 132 : 124);
}

void calculateSamplesPerBit() {
  samplesPerBit = SAMPLE_RATE / (currentTimecode.fps * BITS_PER_FRAME);
  Serial.printf("Client: Samples per bit: %d for %d fps\n", samplesPerBit, currentTimecode.fps);
}

void buildLTCFrame() {
  memset(frameBits, 0, BITS_PER_FRAME);
  
  uint8_t frameUnits = currentTimecode.frames % 10;
  uint8_t frameTens = currentTimecode.frames / 10;
  uint8_t secondUnits = currentTimecode.seconds % 10;
  uint8_t secondTens = currentTimecode.seconds / 10;
  uint8_t minuteUnits = currentTimecode.minutes % 10;
  uint8_t minuteTens = currentTimecode.minutes / 10;
  uint8_t hourUnits = currentTimecode.hours % 10;
  uint8_t hourTens = currentTimecode.hours / 10;
  
  // Frame units (bits 0-3)
  frameBits[0] = (frameUnits >> 0) & 1;
  frameBits[1] = (frameUnits >> 1) & 1;
  frameBits[2] = (frameUnits >> 2) & 1;
  frameBits[3] = (frameUnits >> 3) & 1;
  
  // Frame tens (bits 8-9)
  frameBits[8] = (frameTens >> 0) & 1;
  frameBits[9] = (frameTens >> 1) & 1;
  
  // Drop frame (bit 10)
  bool effectiveDropFrame = currentTimecode.dropFrame;
  if (currentTimecode.fps == 25) {
    effectiveDropFrame = false;
  }
  frameBits[10] = effectiveDropFrame ? 1 : 0;
  frameBits[11] = 0;
  
  // Second units (bits 16-19)
  frameBits[16] = (secondUnits >> 0) & 1;
  frameBits[17] = (secondUnits >> 1) & 1;
  frameBits[18] = (secondUnits >> 2) & 1;
  frameBits[19] = (secondUnits >> 3) & 1;
  
  // Second tens (bits 24-26)
  frameBits[24] = (secondTens >> 0) & 1;
  frameBits[25] = (secondTens >> 1) & 1;
  frameBits[26] = (secondTens >> 2) & 1;
  
  // Minute units (bits 32-35)
  frameBits[32] = (minuteUnits >> 0) & 1;
  frameBits[33] = (minuteUnits >> 1) & 1;
  frameBits[34] = (minuteUnits >> 2) & 1;
  frameBits[35] = (minuteUnits >> 3) & 1;
  
  // Minute tens (bits 40-42)
  frameBits[40] = (minuteTens >> 0) & 1;
  frameBits[41] = (minuteTens >> 1) & 1;
  frameBits[42] = (minuteTens >> 2) & 1;
  
  // Hour units (bits 48-51)
  frameBits[48] = (hourUnits >> 0) & 1;
  frameBits[49] = (hourUnits >> 1) & 1;
  frameBits[50] = (hourUnits >> 2) & 1;
  frameBits[51] = (hourUnits >> 3) & 1;
  
  // Hour tens (bits 56-57)
  frameBits[56] = (hourTens >> 0) & 1;
  frameBits[57] = (hourTens >> 1) & 1;
  
  // Sync word (bits 64-79): 0011111111111101
  frameBits[64] = 0;
  frameBits[65] = 0;
  frameBits[66] = 1;
  frameBits[67] = 1;
  frameBits[68] = 1;
  frameBits[69] = 1;
  frameBits[70] = 1;
  frameBits[71] = 1;
  frameBits[72] = 1;
  frameBits[73] = 1;
  frameBits[74] = 1;
  frameBits[75] = 1;
  frameBits[76] = 1;
  frameBits[77] = 1;
  frameBits[78] = 0;
  frameBits[79] = 1;
}

void IRAM_ATTR onLtcTimer() {
  portENTER_CRITICAL_ISR(&ltcTimerMux);
  
  if (currentTimecode.running && ltcEnabled) {
    uint8_t bit = frameBits[bitIndex];
    
    if (sampleIndex == 0 || (bit == 1 && sampleIndex == (samplesPerBit/2))) {
      currentLevel = !currentLevel;
      dacWriteLevel(currentLevel);
    }
    
    sampleIndex++;
    
    if (sampleIndex >= samplesPerBit) {
      sampleIndex = 0;
      bitIndex++;
      
      if (bitIndex >= BITS_PER_FRAME) {
        bitIndex = 0;
        buildLTCFrame();
      }
    }
  } else {
    dacWrite(OUTPUT_PIN, 128);
  }
  
  portEXIT_CRITICAL_ISR(&ltcTimerMux);
}

void setupLTC() {
  Serial.println("=== Initializing LTC SMPTE Generator ===");
  Serial.printf("Output: GPIO %d (DAC1)\n", OUTPUT_PIN);
  
  calculateSamplesPerBit();
  buildLTCFrame();
  
  ltcTimer = timerBegin(48000);
  timerAttachInterrupt(ltcTimer, &onLtcTimer);
  timerAlarm(ltcTimer, 1, true, 0);
  
  Serial.println("LTC generation ready!");
}

void setup() {
  Serial.begin(115200);
  
  // Generate unique client ID
  clientId = String((uint32_t)ESP.getEfuseMac(), HEX);
  clientId.toUpperCase();
  
  // Initialize hardware
  pinMode(SYNC_LED_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  
  // Initialize preferences
  preferences.begin("timecode_client", false);
  loadSettings();
  
  // Start LTC immediately
  setupLTC();
  
  // NON-BLOCKING WiFi setup
  Serial.println("Starting background WiFi connection...");
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.begin(ssid, password);
  
  // Setup UDP
  setupUDP();
  
  Serial.println("=== Enhanced Timecode Client Started ===");
  Serial.printf("Client ID: %s\n", clientId.c_str());
  Serial.printf("Client Name: %s\n", clientName.c_str());
  Serial.printf("Initial Timecode: %02d:%02d:%02d:%02d @ %dfps %s\n",
                currentTimecode.hours, currentTimecode.minutes,
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "(DROP)" : "(NON-DROP)");
  Serial.printf("Sync Mode: %s\n", getSyncModeString().c_str());
  Serial.println("=== LTC SMPTE Audio Output ACTIVE on GPIO 25 ===");
  
  // Start generating LTC immediately
  currentTimecode.running = true;
  currentTimecode.lastFrameMicros = micros();
  currentTimecode.lastFrameAdvance = micros();
  lastTimecodeUpdate = micros();
}

String getSyncModeString() {
  switch(currentTimecode.syncMode) {
    case SYNC_MODE_HARD: return "HARD";
    case SYNC_MODE_SOFT: return "SOFT"; 
    case SYNC_MODE_AUTO: return "AUTO";
    default: return "UNKNOWN";
  }
}

float getDriftSeconds() {
  return abs(currentTimecode.driftFrames) / (float)currentTimecode.fps;
}

void loadSettings() {
  currentTimecode.fps = preferences.getUInt("fps", 25);
  currentTimecode.dropFrame = preferences.getBool("dropFrame", false);
  
  // Ensure 25fps never has drop frame
  if (currentTimecode.fps == 25) {
    currentTimecode.dropFrame = false;
  }
  
  currentTimecode.hours = preferences.getUInt("hours", 0);
  currentTimecode.minutes = preferences.getUInt("minutes", 0);
  currentTimecode.seconds = preferences.getUInt("seconds", 0);
  currentTimecode.frames = preferences.getUInt("frames", 0);
  currentTimecode.running = preferences.getBool("running", true);
  ltcEnabled = preferences.getBool("ltcEnabled", true);
  
  // Load sync mode
  currentTimecode.syncMode = (SyncMode)preferences.getUInt("syncMode", SYNC_MODE_AUTO);
  
  currentTimecode.lastFrameMicros = micros();
  currentTimecode.lastFrameAdvance = micros();
}

void saveSettings() {
  preferences.putUInt("fps", currentTimecode.fps);
  
  // Ensure we don't save drop frame for 25fps
  bool dropFrameToSave = currentTimecode.dropFrame;
  if (currentTimecode.fps == 25) {
    dropFrameToSave = false;
  }
  preferences.putBool("dropFrame", dropFrameToSave);
  
  preferences.putUInt("hours", currentTimecode.hours);
  preferences.putUInt("minutes", currentTimecode.minutes);
  preferences.putUInt("seconds", currentTimecode.seconds);
  preferences.putUInt("frames", currentTimecode.frames);
  preferences.putBool("running", currentTimecode.running);
  preferences.putBool("ltcEnabled", ltcEnabled);
  
  // Save sync mode
  preferences.putUInt("syncMode", currentTimecode.syncMode);
}

void processSerialCommands() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command.equalsIgnoreCase("SYNC HARD")) {
      currentTimecode.syncMode = SYNC_MODE_HARD;
      saveSettings();
      Serial.println("Sync mode set to HARD");
    } else if (command.equalsIgnoreCase("SYNC SOFT")) {
      currentTimecode.syncMode = SYNC_MODE_SOFT;
      saveSettings();
      Serial.println("Sync mode set to SOFT");
    } else if (command.equalsIgnoreCase("SYNC AUTO")) {
      currentTimecode.syncMode = SYNC_MODE_AUTO;
      saveSettings();
      Serial.println("Sync mode set to AUTO");
    } else if (command.equalsIgnoreCase("SYNC STATUS")) {
      Serial.printf("Current sync mode: %s\n", getSyncModeString().c_str());
      Serial.printf("Current drift: %d frames (%.2f seconds)\n", 
                    currentTimecode.driftFrames, getDriftSeconds());
      Serial.printf("Soft sync active: %s\n", currentTimecode.softSyncing ? "YES" : "NO");
      if (currentTimecode.softSyncing) {
        Serial.printf("Soft sync rate: %.1f%%\n", currentTimecode.syncRateAdjustment * 100.0f);
      }
    } else if (command.equalsIgnoreCase("HELP")) {
      Serial.println("Available commands:");
      Serial.println("  SYNC HARD   - Set sync mode to HARD (immediate sync)");
      Serial.println("  SYNC SOFT   - Set sync mode to SOFT (gradual sync)");
      Serial.println("  SYNC AUTO   - Set sync mode to AUTO (smart sync)");
      Serial.println("  SYNC STATUS - Show current sync status");
      Serial.println("  HELP        - Show this help");
    }
  }
}

void checkWiFiConnection() {
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  static bool wasConnected = false;
  
  unsigned long currentTime = millis();
  
  // Only check WiFi status every 2 seconds
  if (currentTime - lastWiFiCheck < 2000) {
    return;
  }
  lastWiFiCheck = currentTime;
  
  wl_status_t wifiStatus = WiFi.status();
  
  if (wifiStatus == WL_CONNECTED) {
    if (!wasConnected) {
      Serial.println("WiFi connected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      digitalWrite(STATUS_LED_PIN, HIGH);
      wasConnected = true;
    }
    return;
  }
  
  // WiFi not connected - blink status LED
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  if (currentTime - lastBlink > 500) {
    lastBlink = currentTime;
    ledState = !ledState;
    digitalWrite(STATUS_LED_PIN, ledState);
  }
  
  // Try to reconnect only every 30 seconds
  if (currentTime - lastReconnectAttempt > 30000) {
    lastReconnectAttempt = currentTime;
    
    if (wifiStatus != WL_CONNECTED && wifiStatus != WL_IDLE_STATUS) {
      Serial.println("Attempting WiFi reconnection...");
      WiFi.disconnect();
      delay(100);
      WiFi.begin(ssid, password);
    }
  }
  
  if (wasConnected) {
    Serial.println("WiFi disconnected");
    wasConnected = false;
  }
}

void setupUDP() {
  if (udp.begin(UDP_PORT)) {
    Serial.printf("UDP Client Started on port %d\n", UDP_PORT);
  } else {
    Serial.println("Failed to start UDP client!");
  }
}

void sendRegistration() {
  String packet = "REG:" + clientId + ":" + clientName;
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
    }
  }
}

void sendHeartbeat() {
  String packet = "HEARTBEAT:" + clientId;
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
    }
  }
}

void sendStatusUpdate() {
  String packet = "STATUS:" + clientId + ":" + String(currentTimecode.driftFrames) + ":" + String(currentTimecode.lastSequence);
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
    }
  }
}

void sendSyncRequest() {
  String packet = "SYNC_REQUEST:" + clientId;
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
      connectionStats.syncRequests++;
    }
  }
}

void processUDPPackets() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char packetBuffer[255];
    int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
    if (len > 0) {
      packetBuffer[len] = 0;
      String packet = String(packetBuffer);
      lastMasterPacket = millis();
      
      // IMMEDIATELY update connection status for ANY packet from master
      currentTimecode.connectedToMaster = true;
      
      if (packet.startsWith("TC:")) {
        processTimecodePacket(packet);
      } else if (packet.startsWith("SYNC:")) {
        processSyncPacket(packet);
      } else if (packet.startsWith("ACK:")) {
        sendSyncRequest();
      } else if (packet.startsWith("LTC:")) {
        processLTCPacket(packet);
      } else if (packet.startsWith("SYNC_MODE:")) {
        processSyncModePacket(packet);
      }
    }
  }
}

void processSyncModePacket(String packet) {
    String modeStr = packet.substring(10);
    modeStr.trim();
    
    if (modeStr == "HARD") {
        currentTimecode.syncMode = SYNC_MODE_HARD;
        Serial.println("Sync mode set to HARD by master");
    } else if (modeStr == "SOFT") {
        currentTimecode.syncMode = SYNC_MODE_SOFT;
        Serial.println("Sync mode set to SOFT by master");
    } else if (modeStr == "AUTO") {
        currentTimecode.syncMode = SYNC_MODE_AUTO;
        Serial.println("Sync mode set to AUTO by master");
    } else {
        Serial.printf("Unknown sync mode received: %s\n", modeStr.c_str());
        return;
    }
    
    saveSettings();
    sendSyncModeACK();
}

void sendSyncModeACK() {
    String ackPacket = "SYNC_MODE_ACK:" + clientId + ":" + getSyncModeString();
    if (udp.beginPacket(masterIP, UDP_PORT)) {
        udp.write((const uint8_t*)ackPacket.c_str(), ackPacket.length());
        if (udp.endPacket()) {
            connectionStats.packetsSent++;
        }
    }
}

void sendLTCACK(bool state) {
  String ackPacket = "LTC_ACK:" + clientId + ":" + (state ? "1" : "0");
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)ackPacket.c_str(), ackPacket.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
    }
  }
}

void processLTCPacket(String packet) {
  if (packet.length() >= 5) {
    bool newLtcState = (packet.charAt(4) == '1');
    
    if (newLtcState != ltcEnabled) {
      ltcEnabled = newLtcState;
      preferences.putBool("ltcEnabled", ltcEnabled);
      
      if (ltcEnabled) {
        Serial.println("LTC output ENABLED by master");
        bitIndex = 0;
        sampleIndex = 0;
        buildLTCFrame();
      } else {
        Serial.println("LTC output DISABLED by master");
        dacWrite(OUTPUT_PIN, 128);
      }
      
      sendLTCACK(newLtcState);
    }
  }
}

void processTimecodePacket(String packet) {
  String parts[10];
  int partIndex = 0;
  int lastIndex = 0;
  
  for (int i = 0; i < packet.length() && partIndex < 10; i++) {
    if (packet.charAt(i) == ':') {
      parts[partIndex++] = packet.substring(lastIndex, i);
      lastIndex = i + 1;
    }
  }
  if (partIndex < 10 && lastIndex < packet.length()) {
    parts[partIndex] = packet.substring(lastIndex);
    partIndex++;
  }
  
  if (partIndex >= 9) {
    lastMasterTimecode.hours = parts[1].toInt();
    lastMasterTimecode.minutes = parts[2].toInt();
    lastMasterTimecode.seconds = parts[3].toInt();
    lastMasterTimecode.frames = parts[4].toInt();
    lastMasterTimecode.fps = parts[5].toInt();
    
    // Don't allow drop frame for 25fps
    bool receivedDropFrame = (parts[6].toInt() == 1);
    if (lastMasterTimecode.fps == 25) {
      receivedDropFrame = false;
    }
    lastMasterTimecode.dropFrame = receivedDropFrame;
    
    lastMasterTimecode.running = (parts[7].toInt() == 1);
    
    uint32_t sequence = parts[8].toInt();
    unsigned long masterTime = (partIndex >= 10) ? parts[9].toInt() : 0;
    currentTimecode.masterTimeOffset = masterTime - millis();
    
    if (sequence > currentTimecode.lastSequence) {
      currentTimecode.lastSequence = sequence;
    }
    
    // IMMEDIATELY update client running state from master
    if (currentTimecode.connectedToMaster) {
      bool wasRunning = currentTimecode.running;
      currentTimecode.running = lastMasterTimecode.running;
      
      if (wasRunning != currentTimecode.running) {
        Serial.printf("Running state changed to: %s\n", currentTimecode.running ? "RUNNING" : "STOPPED");
      }
    }
    
    // Auto-sync if connected but not synced
    if (currentTimecode.connectedToMaster && !currentTimecode.synced) {
      performSyncBasedOnMode();
    }
    
    calculateDrift();
  }
}

// FIXED: Proper auto mode with 1 second threshold
void performSyncBasedOnMode() {
  uint32_t clientFrames = timecodeToFrames(currentTimecode);
  uint32_t masterFrames = timecodeToFrames(lastMasterTimecode);
  int32_t drift = (int32_t)clientFrames - (int32_t)masterFrames;
  float driftSeconds = abs(drift) / (float)currentTimecode.fps;
  
  Serial.printf("Sync decision: Mode=%s, Drift=%d frames (%.2f seconds)\n", 
                getSyncModeString().c_str(), drift, driftSeconds);
  
  switch(currentTimecode.syncMode) {
    case SYNC_MODE_HARD:
      Serial.println("Performing HARD sync (mode: HARD)");
      doHardSync();
      break;
      
    case SYNC_MODE_SOFT:
      if (abs(drift) <= SOFT_SYNC_THRESHOLD) {
        currentTimecode.synced = true;
        currentTimecode.driftFrames = drift;
        Serial.println("Drift within threshold, marking as synced");
      } else {
        Serial.println("Performing SOFT sync (mode: SOFT)");
        startSoftSync();
      }
      break;
      
    case SYNC_MODE_AUTO:
      // FIXED: Use 1 second threshold as requested
      if (driftSeconds >= AUTO_SYNC_HARD_THRESHOLD) {
        Serial.printf("Performing HARD sync (mode: AUTO, drift >= %.1fs)\n", AUTO_SYNC_HARD_THRESHOLD);
        doHardSync();
      } else {
        if (abs(drift) <= SOFT_SYNC_THRESHOLD) {
          currentTimecode.synced = true;
          currentTimecode.driftFrames = drift;
          Serial.println("Drift within threshold, marking as synced");
        } else {
          Serial.printf("Performing SOFT sync (mode: AUTO, drift < %.1fs)\n", AUTO_SYNC_HARD_THRESHOLD);
          startSoftSync();
        }
      }
      break;
  }
}

void doHardSync() {
  bool fpsChanged = (currentTimecode.fps != lastMasterTimecode.fps);
  
  currentTimecode.hours = lastMasterTimecode.hours;
  currentTimecode.minutes = lastMasterTimecode.minutes;
  currentTimecode.seconds = lastMasterTimecode.seconds;
  currentTimecode.frames = lastMasterTimecode.frames;
  currentTimecode.fps = lastMasterTimecode.fps;
  
  // Don't allow drop frame for 25fps
  if (currentTimecode.fps == 25) {
    currentTimecode.dropFrame = false;
  } else {
    currentTimecode.dropFrame = lastMasterTimecode.dropFrame;
  }
  
  if (currentTimecode.connectedToMaster) {
    currentTimecode.running = lastMasterTimecode.running;
  }
  
  currentTimecode.synced = true;
  currentTimecode.lastFrameMicros = micros();
  currentTimecode.lastFrameAdvance = micros();
  currentTimecode.driftFrames = 0;
  currentTimecode.driftCompensation = 0.0f;
  lastTimecodeUpdate = micros();
  
  // Stop any active soft sync
  currentTimecode.softSyncing = false;
  currentTimecode.syncRateAdjustment = 0.0f;
  
  if (fpsChanged) {
    calculateSamplesPerBit();
  }
  
  buildLTCFrame();
  saveSettings();
  
  Serial.printf("HARD SYNC: %02d:%02d:%02d:%02d @ %dfps %s | Running: %s\n",
                currentTimecode.hours, currentTimecode.minutes,
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "(DROP)" : "(NON-DROP)",
                currentTimecode.running ? "YES" : "NO");
}

void processSyncPacket(String packet) {
  String parts[9];
  int partIndex = 0;
  int lastIndex = 0;
  
  for (int i = 0; i < packet.length() && partIndex < 9; i++) {
    if (packet.charAt(i) == ':') {
      parts[partIndex++] = packet.substring(lastIndex, i);
      lastIndex = i + 1;
    }
  }
  if (partIndex < 9 && lastIndex < packet.length()) {
    parts[partIndex] = packet.substring(lastIndex);
    partIndex++;
  }
  
  if (partIndex >= 8) {
    lastMasterTimecode.hours = parts[1].toInt();
    lastMasterTimecode.minutes = parts[2].toInt();
    lastMasterTimecode.seconds = parts[3].toInt();
    lastMasterTimecode.frames = parts[4].toInt();
    lastMasterTimecode.fps = parts[5].toInt();
    
    // Don't allow drop frame for 25fps
    bool receivedDropFrame = (parts[6].toInt() == 1);
    if (lastMasterTimecode.fps == 25) {
      receivedDropFrame = false;
    }
    lastMasterTimecode.dropFrame = receivedDropFrame;
    
    lastMasterTimecode.running = (parts[7].toInt() == 1);
    
    if (partIndex >= 9) {
      unsigned long masterTime = parts[8].toInt();
      currentTimecode.masterTimeOffset = masterTime - millis();
    }
    
    performSyncBasedOnMode();
    currentTimecode.connectedToMaster = true;
  }
}

// FIXED: Working soft sync that actually changes the timecode speed
void startSoftSync() {
  if (currentTimecode.softSyncing) {
    Serial.println("Soft sync already in progress - continuing");
    return;
  }
  
  uint32_t clientFrames = timecodeToFrames(currentTimecode);
  uint32_t masterFrames = timecodeToFrames(lastMasterTimecode);
  
  int32_t drift = (int32_t)clientFrames - (int32_t)masterFrames;
  
  // Only start soft sync if drift is significant
  if (abs(drift) <= SOFT_SYNC_THRESHOLD) {
    currentTimecode.synced = true;
    currentTimecode.driftFrames = drift;
    return;
  }
  
  // Update FPS and drop frame from master for soft sync
  bool fpsChanged = (currentTimecode.fps != lastMasterTimecode.fps);
  currentTimecode.fps = lastMasterTimecode.fps;
  
  // Don't allow drop frame for 25fps
  if (currentTimecode.fps == 25) {
    currentTimecode.dropFrame = false;
  } else {
    currentTimecode.dropFrame = lastMasterTimecode.dropFrame;
  }
  
  if (fpsChanged) {
    calculateSamplesPerBit();
  }
  
  currentTimecode.softSyncing = true;
  currentTimecode.syncStartTime = millis();
  currentTimecode.syncStartFrames = clientFrames;
  currentTimecode.targetFrames = masterFrames;
  
  // FIXED: Better rate adjustment calculation
  float driftSeconds = abs(drift) / (float)currentTimecode.fps;
  
  // Calculate catch-up time based on drift
  float catchUpTime = 0;
  if (driftSeconds < 2.0) {
    catchUpTime = 3.0; // 3 seconds for small drifts
  } else if (driftSeconds < 10.0) {
    catchUpTime = driftSeconds * 1.5; // 1.5x for medium drifts
  } else {
    catchUpTime = driftSeconds; // 1:1 for large drifts
  }
  
  catchUpTime = constrain(catchUpTime, 2.0f, 15.0f); // 2-15 seconds max
  
  // Calculate required adjustment
  // If client is ahead (positive drift), we need to slow down (negative adjustment)
  // If client is behind (negative drift), we need to speed up (positive adjustment)
  float requiredAdjustment = - (float)drift / (catchUpTime * currentTimecode.fps);
  
  // Clamp the adjustment to reasonable limits
  if (requiredAdjustment > MAX_SYNC_ADJUSTMENT) {
    requiredAdjustment = MAX_SYNC_ADJUSTMENT;
  } else if (requiredAdjustment < -MAX_SYNC_ADJUSTMENT) {
    requiredAdjustment = -MAX_SYNC_ADJUSTMENT;
  }
  
  // Ensure minimum adjustment for very small drifts
  if (abs(requiredAdjustment) < MIN_SYNC_ADJUSTMENT) {
    requiredAdjustment = (drift > 0) ? -MIN_SYNC_ADJUSTMENT : MIN_SYNC_ADJUSTMENT;
  }
  
  currentTimecode.syncRateAdjustment = requiredAdjustment;
  
  Serial.printf("Starting SOFT SYNC: Drift=%d frames (%.2fs), Catch-up=%.1fs, Rate=%.1f%%\n", 
                drift, driftSeconds, catchUpTime, requiredAdjustment * 100.0f);
}

// FIXED: Working soft sync that actually adjusts timecode speed
void updateSoftSync() {
  if (!currentTimecode.softSyncing) return;
  
  unsigned long currentTime = millis();
  
  // Update target frames from latest master timecode
  uint32_t currentMasterFrames = timecodeToFrames(lastMasterTimecode);
  if (currentMasterFrames != currentTimecode.targetFrames) {
    // Master has advanced - update our target
    int32_t oldDrift = (int32_t)timecodeToFrames(currentTimecode) - (int32_t)currentTimecode.targetFrames;
    currentTimecode.targetFrames = currentMasterFrames;
    int32_t newDrift = (int32_t)timecodeToFrames(currentTimecode) - (int32_t)currentTimecode.targetFrames;
    
    // Only restart if drift changed significantly
    if (abs(newDrift - oldDrift) > currentTimecode.fps * 2) { // More than 2 seconds difference
      Serial.printf("Soft sync target updated: old drift=%d, new drift=%d\n", oldDrift, newDrift);
      startSoftSync();
      return;
    }
  }
  
  uint32_t currentFrames = timecodeToFrames(currentTimecode);
  int32_t frameDiff = (int32_t)currentFrames - (int32_t)currentTimecode.targetFrames;
  
  // Check if we've reached the target (within 1 frame)
  if (abs(frameDiff) <= 1) {
    // Soft sync completed successfully
    currentTimecode.softSyncing = false;
    currentTimecode.syncRateAdjustment = 0.0f;
    currentTimecode.synced = true;
    currentTimecode.driftFrames = frameDiff;
    
    Serial.println("SOFT SYNC COMPLETED - Now perfectly synced with master");
    return;
  }
  
  // Check for timeout
  if (currentTime - currentTimecode.syncStartTime > SOFT_SYNC_TIMEOUT) {
    // Soft sync timed out - do hard sync as fallback
    Serial.println("SOFT SYNC TIMEOUT - Falling back to hard sync");
    doHardSync();
    return;
  }
  
  // FIXED: Dynamic adjustment based on current progress
  int32_t remainingDrift = (int32_t)currentTimecode.targetFrames - (int32_t)currentFrames;
  float elapsedSeconds = (currentTime - currentTimecode.syncStartTime) / 1000.0f;
  
  // More aggressive adjustment as we get closer to timeout
  float timeoutProgress = elapsedSeconds / (SOFT_SYNC_TIMEOUT / 1000.0f);
  float timeoutFactor = 1.0f + (timeoutProgress * 1.0f); // 1x to 2x adjustment
  
  // Dynamic adjustment based on remaining time and drift
  float remainingTime = max(1.0f, (SOFT_SYNC_TIMEOUT / 1000.0f) - elapsedSeconds);
  float dynamicAdjustment = - (float)remainingDrift / (remainingTime * currentTimecode.fps) * timeoutFactor;
  
  // Clamp to max adjustment
  if (dynamicAdjustment > MAX_SYNC_ADJUSTMENT) {
    dynamicAdjustment = MAX_SYNC_ADJUSTMENT;
  } else if (dynamicAdjustment < -MAX_SYNC_ADJUSTMENT) {
    dynamicAdjustment = -MAX_SYNC_ADJUSTMENT;
  }
  
  currentTimecode.syncRateAdjustment = dynamicAdjustment;
  
  // Debug output every 2 seconds
  static unsigned long lastSoftSyncDebug = 0;
  if (currentTime - lastSoftSyncDebug > 2000) {
    lastSoftSyncDebug = currentTime;
    float progress = (elapsedSeconds / (SOFT_SYNC_TIMEOUT / 1000.0f)) * 100.0f;
    Serial.printf("Soft Sync: Drift=%d frames, Rate=%.1f%%, Progress=%.1f%%, Time=%lu/%lums\n",
                  remainingDrift, dynamicAdjustment * 100.0f, progress,
                  currentTime - currentTimecode.syncStartTime, SOFT_SYNC_TIMEOUT);
  }
}

uint32_t timecodeToFrames(Timecode tc) {
  return ((tc.hours * 3600) + (tc.minutes * 60) + tc.seconds) * tc.fps + tc.frames;
}

// FIXED: Timecode update that properly applies soft sync speed adjustment
void updateTimecode() {
  if (!currentTimecode.running) return;
  
  unsigned long currentMicros = micros();
  
  // Calculate base frame interval
  float baseInterval = 1000000.0f / currentTimecode.fps;
  
  // Apply soft sync rate adjustment if active - THIS IS WHAT MAKES IT WORK!
  if (currentTimecode.softSyncing) {
    baseInterval = baseInterval * (1.0f - currentTimecode.syncRateAdjustment);
  }
  
  // Apply drift compensation for small corrections (when not soft syncing)
  if (!currentTimecode.softSyncing && currentTimecode.driftCompensation != 0.0f) {
    baseInterval += currentTimecode.driftCompensation;
  }
  
  unsigned long frameInterval = (unsigned long)baseInterval;
  
  if (currentMicros - currentTimecode.lastFrameMicros >= frameInterval) {
    currentTimecode.lastFrameMicros += frameInterval;
    currentTimecode.lastFrameAdvance = currentMicros;
    
    currentTimecode.frames++;
    
    // Only apply drop frame logic for 30fps
    if (currentTimecode.dropFrame && currentTimecode.fps == 30) {
      if (currentTimecode.minutes % 10 != 0 && currentTimecode.seconds == 0 && currentTimecode.frames < 2) {
        currentTimecode.frames = 2;
      }
    }
    
    if (currentTimecode.frames >= currentTimecode.fps) {
      currentTimecode.frames = 0;
      currentTimecode.seconds++;
      
      if (currentTimecode.seconds >= 60) {
        currentTimecode.seconds = 0;
        currentTimecode.minutes++;
        
        if (currentTimecode.minutes >= 60) {
          currentTimecode.minutes = 0;
          currentTimecode.hours++;
          
          if (currentTimecode.hours >= 24) {
            currentTimecode.hours = 0;
          }
        }
      }
    }
    
    buildLTCFrame();
    lastTimecodeUpdate = currentMicros;
  }
}

void calculateDrift() {
  if (!currentTimecode.connectedToMaster) return;
  
  uint32_t clientFrames = timecodeToFrames(currentTimecode);
  uint32_t masterFrames = timecodeToFrames(lastMasterTimecode);
  
  currentTimecode.driftFrames = (int32_t)(clientFrames - masterFrames);
  
  // If we're soft syncing, let updateSoftSync handle the adjustment
  if (currentTimecode.softSyncing) {
    return;
  }
  
  // Only start soft sync if we're not already in one and drift is significant
  // and we're supposed to be synced
  if (currentTimecode.synced && !currentTimecode.softSyncing && abs(currentTimecode.driftFrames) > SOFT_SYNC_THRESHOLD) {
    // Check sync mode to decide what to do
    if (currentTimecode.syncMode == SYNC_MODE_AUTO) {
      float driftSeconds = abs(currentTimecode.driftFrames) / (float)currentTimecode.fps;
      if (driftSeconds < AUTO_SYNC_HARD_THRESHOLD) {
        Serial.printf("Auto mode: Starting soft sync for %.2fs drift\n", driftSeconds);
        startSoftSync();
      } else {
        Serial.printf("Auto mode: Drift %.2fs >= %.1fs threshold, forcing hard sync\n", 
                      driftSeconds, AUTO_SYNC_HARD_THRESHOLD);
        doHardSync();
      }
    } else if (currentTimecode.syncMode == SYNC_MODE_SOFT) {
      Serial.printf("Soft mode: Starting soft sync for %d frames drift\n", currentTimecode.driftFrames);
      startSoftSync();
    }
  }
  
  // Apply gradual drift compensation for small drifts (when not soft syncing)
  if (!currentTimecode.softSyncing && currentTimecode.synced && abs(currentTimecode.driftFrames) > 1) {
    currentTimecode.driftCompensation = currentTimecode.driftFrames * -0.5f;
  } else if (!currentTimecode.softSyncing) {
    currentTimecode.driftCompensation = 0.0f;
  }
}

void sendPeriodicUpdates() {
  unsigned long currentTime = millis();
  
  // Send status update every 10 seconds
  if (currentTime - lastStatusUpdate > 10000) {
    sendStatusUpdate();
    lastStatusUpdate = currentTime;
  }
}

void checkConnection() {
  unsigned long currentTime = millis();
  
  // Check WiFi first
  checkWiFiConnection();
  
  // Check master connection timeout
  if (currentTimecode.connectedToMaster && currentTime - lastMasterPacket > SYNC_TIMEOUT) {
    currentTimecode.connectedToMaster = false;
    currentTimecode.synced = false;
    currentTimecode.driftCompensation = 0.0f;
    currentTimecode.softSyncing = false;
    currentTimecode.syncRateAdjustment = 0.0f;
  }
  
  // More frequent registration when not connected
  if (!currentTimecode.connectedToMaster) {
    if (currentTime - lastRegistration > 2000) {
      if (WiFi.status() == WL_CONNECTED) {
        sendRegistration();
        lastRegistration = currentTime;
      }
    }
  } else {
    // Normal registration interval when connected
    if (currentTime - lastRegistration > REGISTRATION_INTERVAL) {
      if (WiFi.status() == WL_CONNECTED) {
        sendRegistration();
        lastRegistration = currentTime;
      }
    }
  }
  
  // More frequent heartbeats when connected to master
  if (currentTimecode.connectedToMaster && currentTime - lastHeartbeat > HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = currentTime;
  }
  
  // More frequent status updates when connected to master
  if (currentTimecode.connectedToMaster && currentTime - lastStatusUpdate > STATUS_UPDATE_INTERVAL) {
    sendStatusUpdate();
    lastStatusUpdate = currentTime;
  }
}

void checkReconnection() {
  static bool wasConnected = false;
  
  if (!wasConnected && currentTimecode.connectedToMaster) {
    // Just reconnected to master - force immediate sync
    connectionStats.lastReconnection = millis();
    
    // Send sync request immediately
    sendSyncRequest();
    sendRegistration();
  } else if (wasConnected && !currentTimecode.connectedToMaster) {
    Serial.println("Disconnected from master - running independently");
  }
  
  wasConnected = currentTimecode.connectedToMaster;
}

void ensureContinuousOperation() {
  // Always ensure timecode is running when disconnected from master
  if (!currentTimecode.connectedToMaster && !currentTimecode.running) {
    currentTimecode.running = true;
    currentTimecode.lastFrameMicros = micros();
    currentTimecode.lastFrameAdvance = micros();
  }
  
  // Reset drift compensation and soft sync when disconnected
  if (!currentTimecode.connectedToMaster) {
    currentTimecode.driftCompensation = 0.0f;
    currentTimecode.softSyncing = false;
    currentTimecode.syncRateAdjustment = 0.0f;
  }
}

void updateSyncLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  unsigned long currentTime = millis();
  
  if (!currentTimecode.connectedToMaster) {
    // Slow blink when disconnected
    if (currentTime - lastBlink > 1000) {
      lastBlink = currentTime;
      ledState = !ledState;
      digitalWrite(SYNC_LED_PIN, ledState);
    }
  } else if (currentTimecode.softSyncing) {
    // Fast blink when soft syncing
    if (currentTime - lastBlink > 100) {
      lastBlink = currentTime;
      ledState = !ledState;
      digitalWrite(SYNC_LED_PIN, ledState);
    }
  } else if (currentTimecode.synced && abs(currentTimecode.driftFrames) <= 1) {
    // Solid when well-synced
    digitalWrite(SYNC_LED_PIN, HIGH);
  } else {
    // Medium blink when connected but not well-synced
    if (currentTime - lastBlink > 500) {
      lastBlink = currentTime;
      ledState = !ledState;
      digitalWrite(SYNC_LED_PIN, ledState);
    }
  }
}

void updateStatusLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  unsigned long currentTime = millis();
  
  if (WiFi.status() == WL_CONNECTED && currentTimecode.connectedToMaster) {
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else if (WiFi.status() == WL_CONNECTED) {
    // WiFi connected but no master - slow blink
    if (currentTime - lastBlink > 1000) {
      lastBlink = currentTime;
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
    }
  } else {
    // No WiFi - fast blink
    if (currentTime - lastBlink > 500) {
      lastBlink = currentTime;
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
    }
  }
}

void checkAutoResync() {
  static unsigned long lastResyncCheck = 0;
  
  if (!currentTimecode.connectedToMaster) return;
  
  unsigned long currentTime = millis();
  
  // Check if we need to resync every 2 seconds when connected but not synced
  if (currentTime - lastResyncCheck > 2000) {
    lastResyncCheck = currentTime;
    
    if ((!currentTimecode.synced && currentTimecode.connectedToMaster) || 
        abs(currentTimecode.driftFrames) > 10) {
      sendSyncRequest();
    }
  }
}

void loop() {
  // Process serial commands for sync mode changes
  processSerialCommands();
  
  // Network operations
  processUDPPackets();
  checkConnection();
  
  // More frequent reconnection checks
  static unsigned long lastReconnectCheck = 0;
  if (millis() - lastReconnectCheck > 1000) {
    lastReconnectCheck = millis();
    checkReconnection();
    checkAutoResync();
  }
  
  ensureContinuousOperation();
  
  // Update soft sync if active
  if (currentTimecode.softSyncing) {
    updateSoftSync();
  }
  
  // Update timecode (advances frames)
  updateTimecode();
  
  // Update status indicators
  updateStatusLED();
  updateSyncLED();
  
  // Connection status output
  static unsigned long lastConnectionOutput = 0;
  if (millis() - lastConnectionOutput > 2000) {
    lastConnectionOutput = millis();
    
    if (currentTimecode.softSyncing) {
      Serial.printf("Soft Sync Active: Rate=%.1f%%, Drift=%d frames\n",
                    currentTimecode.syncRateAdjustment * 100.0f,
                    currentTimecode.driftFrames);
    }
  }
  
  // Full debug output less frequently
  static unsigned long lastSerialOutput = 0;
  if (millis() - lastSerialOutput > 5000) {
    lastSerialOutput = millis();
    
    if (currentTimecode.softSyncing) {
      Serial.printf("SOFT SYNC: Timecode: %02d:%02d:%02d:%02d | Rate: %.1f%% | Drift: %d frames\n",
                    currentTimecode.hours, currentTimecode.minutes,
                    currentTimecode.seconds, currentTimecode.frames,
                    currentTimecode.syncRateAdjustment * 100.0f,
                    currentTimecode.driftFrames);
    } else {
      Serial.printf("Timecode: %02d:%02d:%02d:%02d | Mode:%s | WiFi:%s | Master:%s | Synced:%s | Drift:%d (%.2fs)\n",
                    currentTimecode.hours, currentTimecode.minutes,
                    currentTimecode.seconds, currentTimecode.frames,
                    getSyncModeString().c_str(),
                    WiFi.status() == WL_CONNECTED ? "OK" : "NO",
                    currentTimecode.connectedToMaster ? "OK" : "NO",
                    currentTimecode.synced ? "YES" : "NO",
                    currentTimecode.driftFrames,
                    getDriftSeconds());
    }
  }
  
  delay(10);
}