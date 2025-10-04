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

// Enhanced Timecode structure - SIMPLIFIED (no soft sync)
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
  unsigned long lastStableUpdate; // For stable timing
  uint32_t totalFrames; // Track total frames for better timing
  unsigned long lastSyncTime; // Track when last synced with master
};

Timecode currentTimecode = {0, 0, 0, 0, 25, false, true, false, false, 0, 0, 0, 0, 0, 0, 0};
Timecode lastMasterTimecode = {0, 0, 0, 0, 25, false, false, false, false, 0, 0, 0, 0};

// Client identification
String clientId;
String clientName = "Camera_01";

// Physical pins
const int SYNC_LED_PIN = 2;
const int STATUS_LED_PIN = 4;

// Enhanced timing constants
const unsigned long REGISTRATION_INTERVAL = 5000; // Reduced from 10000
const unsigned long HEARTBEAT_INTERVAL = 2000;
const unsigned long STATUS_UPDATE_INTERVAL = 2000;
const unsigned long SYNC_TIMEOUT = 5000;

// Hard sync threshold (frames) - only sync if drift is more than this
const int HARD_SYNC_THRESHOLD = 3;

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

// UPDATED: WiFi event handler with correct constants
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi Connected to AP");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("Got IP: ");
      Serial.println(WiFi.localIP());
      digitalWrite(STATUS_LED_PIN, HIGH);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi Disconnected");
      digitalWrite(STATUS_LED_PIN, LOW);
      currentTimecode.connectedToMaster = false;
      currentTimecode.synced = false;
      break;
    default:
      break;
  }
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
  
  // IMPROVED WiFi setup with event handler
  Serial.println("Starting WiFi connection...");
  WiFi.disconnect(true);
  delay(100);
  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  // Set a hostname for easier identification
  String hostname = "TimecodeClient-" + clientId;
  WiFi.setHostname(hostname.c_str());
  
  Serial.printf("Connecting to: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  // Wait a bit for connection (non-blocking but gives time for initial connection)
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    Serial.println("WiFi connection failed, will retry in background");
  }
  
  // Setup UDP
  setupUDP();
  
  Serial.println("=== Enhanced Timecode Client Started ===");
  Serial.printf("Client ID: %s\n", clientId.c_str());
  Serial.printf("Client Name: %s\n", clientName.c_str());
  Serial.printf("Hostname: %s\n", hostname.c_str());
  Serial.printf("Initial Timecode: %02d:%02d:%02d:%02d @ %dfps %s\n",
                currentTimecode.hours, currentTimecode.minutes,
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "(DROP)" : "(NON-DROP)");
  Serial.println("=== LTC SMPTE Audio Output ACTIVE on GPIO 25 ===");
  
  // Start generating LTC immediately
  currentTimecode.running = true;
  currentTimecode.lastFrameMicros = micros();
  currentTimecode.lastStableUpdate = micros(); // Initialize stable timing
  lastTimecodeUpdate = micros();
  
  // Send initial registration immediately
  if (WiFi.status() == WL_CONNECTED) {
    sendRegistration();
    lastRegistration = millis();
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
  
  currentTimecode.lastFrameMicros = micros();
  currentTimecode.lastStableUpdate = micros(); // Initialize stable timing
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
}

void processSerialCommands() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command.equalsIgnoreCase("SYNC")) {
      sendSyncRequest();
      Serial.println("Sync request sent to master");
    } else if (command.equalsIgnoreCase("STATUS")) {
      Serial.printf("Current drift: %d frames (%.2f seconds)\n", 
                    currentTimecode.driftFrames, getDriftSeconds());
      Serial.printf("Connected to master: %s\n", currentTimecode.connectedToMaster ? "YES" : "NO");
      Serial.printf("Synced: %s\n", currentTimecode.synced ? "YES" : "NO");
      Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      }
    } else if (command.equalsIgnoreCase("HELP")) {
      Serial.println("Available commands:");
      Serial.println("  SYNC   - Request sync from master");
      Serial.println("  STATUS - Show current sync status");
      Serial.println("  HELP   - Show this help");
    }
  }
}

// IMPROVED WiFi connection check
void checkWiFiConnection() {
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  static bool wasConnected = false;
  
  unsigned long currentTime = millis();
  
  // Only check WiFi status every 5 seconds
  if (currentTime - lastWiFiCheck < 5000) {
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
      
      // Send registration immediately when reconnected
      sendRegistration();
      lastRegistration = currentTime;
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
  
  // Try to reconnect only every 10 seconds (reduced from 30)
  if (currentTime - lastReconnectAttempt > 10000) {
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
    currentTimecode.connectedToMaster = false;
    currentTimecode.synced = false;
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
  Serial.printf("Sending registration: %s\n", packet.c_str());
  
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
      Serial.println("Registration packet sent successfully");
    } else {
      Serial.println("Failed to send registration packet");
    }
  } else {
    Serial.println("Failed to begin UDP packet for registration");
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
  Serial.printf("Sending sync request: %s\n", packet.c_str());
  
  if (udp.beginPacket(masterIP, UDP_PORT)) {
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    if (udp.endPacket()) {
      connectionStats.packetsSent++;
      connectionStats.syncRequests++;
      Serial.println("Sync request sent successfully");
    } else {
      Serial.println("Failed to send sync request");
    }
  } else {
    Serial.println("Failed to begin UDP packet for sync request");
  }
}

// IMPROVED UDP packet processing
void processUDPPackets() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char packetBuffer[255];
    int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
    if (len > 0) {
      packetBuffer[len] = 0;
      String packet = String(packetBuffer);
      IPAddress remoteIP = udp.remoteIP();
      
      Serial.printf("Received UDP packet from %s: %s\n", remoteIP.toString().c_str(), packet.c_str());
      
      lastMasterPacket = millis();
      
      // Update connection status for ANY packet from master
      if (!currentTimecode.connectedToMaster) {
        currentTimecode.connectedToMaster = true;
        Serial.println("Connected to master!");
      }
      
      if (packet.startsWith("TC:")) {
        processTimecodePacket(packet);
      } else if (packet.startsWith("SYNC:")) {
        processSyncPacket(packet);
      } else if (packet.startsWith("ACK:")) {
        // Acknowledge registration - send sync request
        Serial.println("Received ACK from master - sending sync request");
        sendSyncRequest();
      } else if (packet.startsWith("LTC:")) {
        processLTCPacket(packet);
      } else {
        Serial.printf("Unknown packet type: %s\n", packet.c_str());
      }
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
    
    // Update client running state from master (but don't let it interfere with stable timing)
    if (currentTimecode.connectedToMaster) {
      bool wasRunning = currentTimecode.running;
      currentTimecode.running = lastMasterTimecode.running;
      
      if (wasRunning != currentTimecode.running) {
        Serial.printf("Running state changed to: %s\n", currentTimecode.running ? "RUNNING" : "STOPPED");
      }
    }
    
    // Calculate drift but DON'T auto-sync - only sync when explicitly requested
    calculateDrift();
  }
}

// SIMPLIFIED: Only hard sync when explicitly requested via SYNC packet
void performHardSyncIfNeeded() {
  if (!currentTimecode.connectedToMaster) return;
  
  uint32_t clientFrames = timecodeToFrames(currentTimecode);
  uint32_t masterFrames = timecodeToFrames(lastMasterTimecode);
  int32_t drift = (int32_t)clientFrames - (int32_t)masterFrames;
  
  // Only sync if drift is significant (more than threshold)
  if (abs(drift) > HARD_SYNC_THRESHOLD) {
    Serial.printf("Performing HARD sync: Drift=%d frames (%.2fs)\n", drift, abs(drift) / (float)currentTimecode.fps);
    doHardSync();
  } else {
    // If we're within threshold, mark as synced but don't change timecode
    currentTimecode.synced = true;
    currentTimecode.driftFrames = drift;
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
  currentTimecode.lastStableUpdate = micros(); // Reset stable timing
  currentTimecode.driftFrames = 0;
  currentTimecode.totalFrames = timecodeToFrames(currentTimecode); // Reset total frames
  currentTimecode.lastSyncTime = millis(); // Track when we last synced
  lastTimecodeUpdate = micros();
  
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
    
    // When we receive a sync packet, always perform hard sync
    Serial.println("Received SYNC packet from master - performing hard sync");
    doHardSync();
    currentTimecode.connectedToMaster = true;
  }
}

uint32_t timecodeToFrames(Timecode tc) {
  return ((tc.hours * 3600) + (tc.minutes * 60) + tc.seconds) * tc.fps + tc.frames;
}

// STABLE timecode update with proper frame boundary checking - UPDATED FOR BETTER STABILITY
void updateTimecode() {
  if (!currentTimecode.running) return;
  
  unsigned long currentMicros = micros();
  
  // Calculate base frame interval with high precision
  double baseInterval = 1000000.0 / (double)currentTimecode.fps;
  
  // Use more precise timing with accumulated frames approach
  // Calculate how many frames should have passed since last update
  unsigned long elapsedMicros = currentMicros - currentTimecode.lastStableUpdate;
  uint32_t expectedFrames = elapsedMicros / (unsigned long)(baseInterval);
  
  if (expectedFrames > 0) {
    // Advance by the exact number of frames that should have passed
    for (uint32_t i = 0; i < expectedFrames; i++) {
      advanceOneFrame();
    }
    
    // Update the stable timing reference - CRITICAL FIX
    // Use precise calculation to avoid accumulated error
    currentTimecode.lastStableUpdate += (unsigned long)(expectedFrames * baseInterval);
    currentTimecode.totalFrames += expectedFrames;
    
    // Only build LTC frame when we actually advance frames
    buildLTCFrame();
    lastTimecodeUpdate = currentMicros;
  }
  
  // Reset stable update if we get too far behind (prevent overflow)
  if (currentMicros - currentTimecode.lastStableUpdate > 1000000) { // 1 second
    currentTimecode.lastStableUpdate = currentMicros;
  }
}

// Single frame advancement with strict boundary checking
void advanceOneFrame() {
  currentTimecode.frames++;
  
  // STRICT frame boundary checking - never allow frames >= fps
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
    
    // Only apply drop frame logic for 30fps after second rollover
    if (currentTimecode.dropFrame && currentTimecode.fps == 30) {
      // Drop frame logic: skip frames 0 and 1 at the start of each minute, except minutes 0, 10, 20, 30, 40, 50
      if (currentTimecode.minutes % 10 != 0 && currentTimecode.seconds == 0 && currentTimecode.frames < 2) {
        currentTimecode.frames = 2;
      }
    }
  }
}

void calculateDrift() {
  if (!currentTimecode.connectedToMaster) return;
  
  uint32_t clientFrames = timecodeToFrames(currentTimecode);
  uint32_t masterFrames = timecodeToFrames(lastMasterTimecode);
  
  currentTimecode.driftFrames = (int32_t)(clientFrames - masterFrames);
  
  // Don't auto-sync based on drift calculation - only sync when explicitly requested via SYNC packet
  // This prevents constant adjustments that cause instability
}

void sendPeriodicUpdates() {
  unsigned long currentTime = millis();
  
  // Send status update every 10 seconds
  if (currentTime - lastStatusUpdate > 10000) {
    sendStatusUpdate();
    lastStatusUpdate = currentTime;
  }
}

// IMPROVED connection management
void checkConnection() {
  unsigned long currentTime = millis();
  
  // Check WiFi first
  checkWiFiConnection();
  
  // Check master connection timeout
  if (currentTimecode.connectedToMaster && currentTime - lastMasterPacket > SYNC_TIMEOUT) {
    currentTimecode.connectedToMaster = false;
    currentTimecode.synced = false;
    Serial.println("Master connection timeout - running independently");
  }
  
  // More frequent registration when not connected
  if (!currentTimecode.connectedToMaster) {
    if (currentTime - lastRegistration > 2000 && WiFi.status() == WL_CONNECTED) {
      sendRegistration();
      lastRegistration = currentTime;
    }
  } else {
    // Normal registration interval when connected
    if (currentTime - lastRegistration > REGISTRATION_INTERVAL && WiFi.status() == WL_CONNECTED) {
      sendRegistration();
      lastRegistration = currentTime;
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
    // Just reconnected to master - check if we need to sync
    connectionStats.lastReconnection = millis();
    
    // Calculate drift but don't auto-sync - wait for explicit sync packet
    calculateDrift();
    
    // Send sync request if we're not synced or have significant drift
    if (!currentTimecode.synced || abs(currentTimecode.driftFrames) > HARD_SYNC_THRESHOLD) {
      sendSyncRequest();
    }
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
    currentTimecode.lastStableUpdate = micros(); // Reset stable timing
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
  
  // Check if we need to resync every 30 seconds when connected
  if (currentTime - lastResyncCheck > 30000) {
    lastResyncCheck = currentTime;
    
    // Only resync if we're significantly out of sync
    if ((!currentTimecode.synced || abs(currentTimecode.driftFrames) > HARD_SYNC_THRESHOLD)) {
      sendSyncRequest();
      Serial.println("Auto-resync requested due to significant drift");
    }
  }
}

void loop() {
  // Process serial commands
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
  
  // Update timecode (advances frames) - STABLE operation
  updateTimecode();
  
  // Update status indicators
  updateStatusLED();
  updateSyncLED();
  
  // Full debug output less frequently
  static unsigned long lastSerialOutput = 0;
  if (millis() - lastSerialOutput > 5000) {
    lastSerialOutput = millis();
    
    Serial.printf("Timecode: %02d:%02d:%02d:%02d | WiFi:%s | Master:%s | Synced:%s | Drift:%d (%.2fs)\n",
                  currentTimecode.hours, currentTimecode.minutes,
                  currentTimecode.seconds, currentTimecode.frames,
                  WiFi.status() == WL_CONNECTED ? "OK" : "NO",
                  currentTimecode.connectedToMaster ? "OK" : "NO",
                  currentTimecode.synced ? "YES" : "NO",
                  currentTimecode.driftFrames,
                  getDriftSeconds());
  }
  
  delay(10);
}