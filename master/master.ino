#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// LTC SMPTE Audio Configuration
#define SAMPLE_RATE     48000
#define OUTPUT_PIN      25   // DAC1 (GPIO25)
#define BITS_PER_FRAME  80
unsigned long lastAutoSave = 0;
const unsigned long AUTO_SAVE_INTERVAL = 5000; // 15 seconds
// LTC variables
uint8_t frameBits[BITS_PER_FRAME];
int bitIndex = 0;
int sampleIndex = 0;
bool currentLevel = false;
bool ltcEnabled = true;
int samplesPerBit = 0;

hw_timer_t *ltcTimer = NULL;
portMUX_TYPE ltcTimerMux = portMUX_INITIALIZER_UNLOCKED;

// Configuration
const char* ssid = "TimecodeMaster";
const char* password = "timecode123";

AsyncWebServer server(80);
Preferences preferences;
WiFiUDP udp;

// Network configuration
const unsigned int UDP_PORT = 12345;
IPAddress broadcastIP(255, 255, 255, 255);

// Enhanced Timecode structure with better timing
struct Timecode {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
  uint8_t frames;
  uint8_t fps;
  bool dropFrame;
  bool running;
  uint32_t sequence;
  unsigned long lastFrameMicros;
  unsigned long startTime;
  float driftCompensation; // New: For gradual drift correction
};

Timecode currentTimecode = {0, 0, 0, 0, 25, false, true, 0, 0, 0, 0.0};

// Enhanced Client management
struct ClientInfo {
  String id;
  String name;
  IPAddress ip;
  bool connected;
  bool synced;
  int32_t driftFrames;
  unsigned long lastSeen;
  uint32_t lastSequence;
  unsigned long lastResponse;
  uint16_t packetLoss; // New: Track client reliability
  int8_t syncPriority; // New: Priority for sync order
  String syncMode;  // ADD THIS
};

std::vector<ClientInfo> clients;
const unsigned long CLIENT_TIMEOUT = 3000; // Increased to 10 seconds
const unsigned long CLIENT_CLEANUP_INTERVAL = 60000; // Cleanup every minute

// Enhanced timing constants
const unsigned long FRAME_BROADCAST_INTERVAL = 40;
const unsigned long SYNC_BROADCAST_INTERVAL = 0; // REMOVED: No periodic syncs
const unsigned long HEARTBEAT_INTERVAL = 5000; // Reduced frequency

// Physical pins
const int SYNC_BUTTON_PIN = 0;
const int STATUS_LED_PIN = 2;
bool lastButtonState = HIGH;

// Enhanced packet management
uint32_t packetSequence = 0;
unsigned long lastBroadcast = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastClientCleanup = 0;

// Connection quality monitoring
struct NetworkStats {
  uint32_t packetsSent;
  uint32_t packetsReceived;
  uint32_t packetsLost;
  unsigned long lastStatsUpdate;
};

NetworkStats networkStats = {0, 0, 0, 0};

// Function declarations
void calculateSamplesPerBit();
void buildLTCFrame();
void IRAM_ATTR onLtcTimer();
void setupLTC();
void setupWiFi();
void setupUDP();
void setupWebServer();
void loadSettings();
void saveSettings();
void updateTimecode();
void broadcastTimecode();
void manualSyncAllClients();
void processUDPPackets();
void processRegistration(String packet, IPAddress clientIP);
void processStatusUpdate(String packet, IPAddress clientIP);
void processHeartbeat(String packet, IPAddress clientIP);
void processSyncRequest(String packet, IPAddress clientIP);
void processAck(String packet, IPAddress clientIP);
void sendDirectSync(IPAddress clientIP);
void checkClientTimeouts();
void cleanupDisconnectedClients();
void checkSyncButton();
void updateStatusLED();
String getClientsJSON();
String getNetworkStatsJSON();
void updateNetworkStats(bool packetSent, bool packetReceived);
void advanceTimecodeByMillis(unsigned long millisToAdvance); // ADDED: Missing function declaration
void conditionalSaveSettings();
void broadcastImmediateStateChange(); // NEW: Immediate state broadcast
void sendStateToAllClients(); // NEW: Send current state to all clients

// Enhanced audio output with better levels
inline void dacWriteLevel(bool level) {
  // Reduced to 1/5 power - much quieter for camera audio input
  dacWrite(OUTPUT_PIN, level ? 132 : 124); // ~0.1V peak-to-peak (was 150:106)
}

void calculateSamplesPerBit() {
  samplesPerBit = SAMPLE_RATE / (currentTimecode.fps * BITS_PER_FRAME);
  Serial.printf("Master: Samples per bit: %d for %d fps\n", samplesPerBit, currentTimecode.fps);
}

void conditionalSaveSettings() {
  static unsigned long lastSaveTime = 0;
  static Timecode lastSavedState = {0, 0, 0, 0, 25, false, true, 0, 0, 0, 0.0};
  
  // Check if any timecode values have actually changed
  bool timecodeChanged = (lastSavedState.hours != currentTimecode.hours ||
                         lastSavedState.minutes != currentTimecode.minutes ||
                         lastSavedState.seconds != currentTimecode.seconds ||
                         lastSavedState.frames != currentTimecode.frames ||
                         lastSavedState.fps != currentTimecode.fps ||
                         lastSavedState.dropFrame != currentTimecode.dropFrame ||
                         lastSavedState.running != currentTimecode.running);
  
  // Only save if something changed or it's been more than 30 seconds
  unsigned long currentTime = millis();
  if (timecodeChanged || (currentTime - lastSaveTime > 30000)) {
    saveSettings();
    lastSaveTime = currentTime;
    lastSavedState = currentTimecode;
  }
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
  
  // User bits 1 (bits 4-7) - all zeros
  
  // Frame tens (bits 8-9)
  frameBits[8] = (frameTens >> 0) & 1;
  frameBits[9] = (frameTens >> 1) & 1;
  
  // Drop frame (bit 10) and color frame (bit 11)
  frameBits[10] = currentTimecode.dropFrame ? 1 : 0;
  frameBits[11] = 0;
  
  // User bits 2 (bits 12-15) - all zeros
  
  // Second units (bits 16-19)
  frameBits[16] = (secondUnits >> 0) & 1;
  frameBits[17] = (secondUnits >> 1) & 1;
  frameBits[18] = (secondUnits >> 2) & 1;
  frameBits[19] = (secondUnits >> 3) & 1;
  
  // User bits 3 (bits 20-23) - all zeros
  
  // Second tens (bits 24-26)
  frameBits[24] = (secondTens >> 0) & 1;
  frameBits[25] = (secondTens >> 1) & 1;
  frameBits[26] = (secondTens >> 2) & 1;
  
  // Binary group flag 1 (bit 27)
  frameBits[27] = 0;
  
  // User bits 4 (bits 28-31) - all zeros
  
  // Minute units (bits 32-35)
  frameBits[32] = (minuteUnits >> 0) & 1;
  frameBits[33] = (minuteUnits >> 1) & 1;
  frameBits[34] = (minuteUnits >> 2) & 1;
  frameBits[35] = (minuteUnits >> 3) & 1;
  
  // User bits 5 (bits 36-39) - all zeros
  
  // Minute tens (bits 40-42)
  frameBits[40] = (minuteTens >> 0) & 1;
  frameBits[41] = (minuteTens >> 1) & 1;
  frameBits[42] = (minuteTens >> 2) & 1;
  
  // Binary group flag 2 (bit 43)
  frameBits[43] = 0;
  
  // User bits 6 (bits 44-47) - all zeros
  
  // Hour units (bits 48-51)
  frameBits[48] = (hourUnits >> 0) & 1;
  frameBits[49] = (hourUnits >> 1) & 1;
  frameBits[50] = (hourUnits >> 2) & 1;
  frameBits[51] = (hourUnits >> 3) & 1;
  
  // User bits 7 (bits 52-55) - all zeros
  
  // Hour tens (bits 56-57)
  frameBits[56] = (hourTens >> 0) & 1;
  frameBits[57] = (hourTens >> 1) & 1;
  
  // Reserved (bit 58)
  frameBits[58] = 0;
  
  // Binary group flag 3 (bit 59)
  frameBits[59] = 0;
  
  // User bits 8 (bits 60-63) - all zeros
  
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

// ADDED: Missing function that was causing the compilation error
void advanceTimecodeByMillis(unsigned long millisToAdvance) {
  if (millisToAdvance == 0) return;
  
  // Calculate how many frames to advance based on current FPS
  unsigned long framesToAdvance = (millisToAdvance * currentTimecode.fps) / 1000;
  
  Serial.printf("Advancing timecode by %lu frames (%lu ms at %d fps)\n", 
                framesToAdvance, millisToAdvance, currentTimecode.fps);
  
  // Advance by frames
  for (unsigned long i = 0; i < framesToAdvance; i++) {
    currentTimecode.frames++;
    
    // Handle frame rollover
    if (currentTimecode.frames >= currentTimecode.fps) {
      currentTimecode.frames = 0;
      currentTimecode.seconds++;
      
      // Handle second rollover
      if (currentTimecode.seconds >= 60) {
        currentTimecode.seconds = 0;
        currentTimecode.minutes++;
        
        // Handle minute rollover
        if (currentTimecode.minutes >= 60) {
          currentTimecode.minutes = 0;
          currentTimecode.hours++;
          
          // Handle hour rollover
          if (currentTimecode.hours >= 24) {
            currentTimecode.hours = 0;
          }
        }
      }
    }
  }
}

// NEW: Immediate state change broadcast
void broadcastImmediateStateChange() {
  String packet = "TC:";
  packet += String(currentTimecode.hours) + ":";
  packet += String(currentTimecode.minutes) + ":";
  packet += String(currentTimecode.seconds) + ":";
  packet += String(currentTimecode.frames) + ":";
  packet += String(currentTimecode.fps) + ":";
  packet += String(currentTimecode.dropFrame ? "1" : "0") + ":";
  packet += String(currentTimecode.running ? "1" : "0") + ":";
  packet += String(packetSequence) + ":";
  packet += String(millis());
  
  // Send multiple times for reliability
  for (int i = 0; i < 3; i++) {
    udp.beginPacket(broadcastIP, UDP_PORT);
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    udp.endPacket();
    delay(5);
  }
  
  Serial.printf("Immediate state broadcast: %s\n", packet.c_str());
}

// NEW: Send state to all clients directly
void sendStateToAllClients() {
  String packet = "TC:";
  packet += String(currentTimecode.hours) + ":";
  packet += String(currentTimecode.minutes) + ":";
  packet += String(currentTimecode.seconds) + ":";
  packet += String(currentTimecode.frames) + ":";
  packet += String(currentTimecode.fps) + ":";
  packet += String(currentTimecode.dropFrame ? "1" : "0") + ":";
  packet += String(currentTimecode.running ? "1" : "0") + ":";
  packet += String(packetSequence) + ":";
  packet += String(millis());
  
  for (auto &client : clients) {
    if (client.connected) {
      udp.beginPacket(client.ip, UDP_PORT);
      udp.write((const uint8_t*)packet.c_str(), packet.length());
      udp.endPacket();
    }
  }
}

const char HTML_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <title>Timecode Master - Enhanced</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
        .container { max-width: 900px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
        .card { background: #f9f9f9; padding: 20px; margin: 15px 0; border-radius: 8px; border-left: 4px solid #4CAF50; }
        .timecode { font-size: 2.5em; font-family: 'Courier New', monospace; text-align: center; background: #000; color: #0f0; padding: 15px; border-radius: 5px; margin: 10px 0; }
        button { padding: 12px 20px; margin: 8px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; font-weight: bold; }
        .btn-start { background: #4CAF50; color: white; }
        .btn-stop { background: #f44336; color: white; }
        .btn-sync { background: #2196F3; color: white; }
        .btn-enable { background: #4CAF50; color: white; }
        .btn-disable { background: #ff9800; color: white; }
        button:hover { opacity: 0.9; }
        .client { padding: 15px; margin: 10px 0; background: #fff; border-radius: 5px; border-left: 5px solid #f44336; }
        .client.connected { border-left-color: #4CAF50; }
        .status-led { display: inline-block; width: 12px; height: 12px; border-radius: 50%; margin-right: 10px; }
        .led-connected { background: #4CAF50; }
        .led-disconnected { background: #ccc; }
        .drift { padding: 4px 8px; border-radius: 12px; font-size: 0.8em; margin-left: 10px; color: white; }
        .drift-good { background: #4CAF50; }
        .drift-warning { background: #ff9800; }
        .drift-bad { background: #f44336; }
        .config-group { margin: 10px 0; }
        input, select { padding: 8px; margin: 5px; }
        .ltc-status { background: #e3f2fd; padding: 10px; border-radius: 5px; margin: 10px 0; }
        .stats-card { background: #e8f5e9; padding: 15px; border-radius: 5px; margin: 10px 0; }
        .network-stats { font-family: 'Courier New', monospace; background: #f5f5f5; padding: 10px; border-radius: 5px; }
        .drop-frame-disabled { color: #999; }
        .sync-mode-controls { background: #fff3cd; padding: 10px; border-radius: 5px; margin: 10px 0; border-left: 4px solid #ffc107; }
        .sync-mode-btn { background: #6c757d; color: white; padding: 8px 15px; margin: 3px; border: none; border-radius: 4px; cursor: pointer; }
        .sync-mode-btn.active { background: #007bff; }
        .sync-mode-btn.soft { background: #28a745; }
        .sync-mode-btn.auto { background: #17a2b8; }
        .sync-mode-btn.hard { background: #dc3545; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Timecode Master - Enhanced</h1>
        
        <div class="stats-card">
            <h3>Network Statistics</h3>
            <div class="network-stats" id="networkStats">
                Loading...
            </div>
        </div>
        
        <div class="ltc-status" id="ltcStatusCard">
            <h3>LTC SMPTE Audio Output</h3>
            <p><strong>Status:</strong> <span id="ltcStatus">Active</span></p>
            <p><strong>Output:</strong> GPIO 25 (DAC1) | <strong>Format:</strong> <span id="ltcFormat">25fps @ 48kHz</span></p>
            <div>
                <button class="btn-enable" onclick="toggleLTC(true)">Enable LTC</button>
                <button class="btn-disable" onclick="toggleLTC(false)">Disable LTC</button>
            </div>
        </div>
        
        <div class="card">
            <h2>Current Timecode</h2>
            <div class="timecode" id="currentTimecode">00:00:00:00</div>
            <div style="text-align: center;">
                <button class="btn-start" onclick="sendCommand('start')">Start</button>
                <button class="btn-stop" onclick="sendCommand('stop')">Stop</button>
                <button class="btn-sync" onclick="sendCommand('sync')">Sync All</button>
                <button onclick="sendCommand('reset')">Reset</button>
            </div>
        </div>

        <div class="card">
            <h2>Configuration</h2>
            <div class="config-group">
                <label>Frame Rate:</label>
                <select id="fpsSelect" onchange="updateFPS()">
                    <option value="24">24 fps (Film)</option>
                    <option value="25">25 fps (PAL)</option>
                    <option value="30">30 fps (NTSC)</option>
                </select>
            </div>
            
            <div class="config-group">
                <label id="dropFrameLabel">
                    <input type="checkbox" id="dropFrame" onchange="updateDropFrame()">
                    Drop Frame
                </label>
                <div id="dropFrameNote" style="font-size: 0.9em; color: #666; display: none;">
                    Note: Drop frame is only applicable to 30fps (29.97fps) and 24fpr (23.976fps)
                </div>
            </div>
            
            <div class="config-group">
                <label>Set Timecode:</label>
                <input type="number" id="hours" min="0" max="23" value="0" style="width: 60px;">:
                <input type="number" id="minutes" min="0" max="59" value="0" style="width: 60px;">:
                <input type="number" id="seconds" min="0" max="59" value="0" style="width: 60px;">:
                <input type="number" id="frames" min="0" max="59" value="0" style="width: 60px;">
                <button onclick="setTimecode()">Set</button>
            </div>
        </div>

        <div class="card">
            <h2>Client Sync Mode Control</h2>
            <div class="sync-mode-controls">
                <p><strong>Set sync mode for all connected clients:</strong></p>
                <div>
                    <button class="sync-mode-btn hard" onclick="setAllClientsSyncMode('HARD')">HARD SYNC</button>
                    <button class="sync-mode-btn soft" onclick="setAllClientsSyncMode('SOFT')">SOFT SYNC</button>
                    <button class="sync-mode-btn auto" onclick="setAllClientsSyncMode('AUTO')">AUTO SYNC</button>
                </div>
                <p style="font-size: 0.9em; margin-top: 10px;">
                    <strong>HARD</strong>: Immediate sync<br>
                    <strong>SOFT</strong>: Gradual speed adjustment<br>
                    <strong>AUTO</strong>: Soft sync (&lt;3s drift) / Hard sync (&ge;3s drift)
                </p>
            </div>
        </div>

        <div class="card">
            <h2>Connected Clients</h2>
            <div class="client-list" id="clientList">
                <div>No clients connected</div>
            </div>
        </div>
    </div>

    <script>
        function sendCommand(cmd) {
            fetch('/command?cmd=' + cmd)
                .then(response => response.json())
                .then(data => {
                    console.log('Command response:', data);
                    updateTimecodeDisplay();
                })
                .catch(error => console.error('Error:', error));
        }
        
        function updateFPS() {
            const fps = document.getElementById('fpsSelect').value;
            fetch('/config?fps=' + fps)
                .then(response => response.json())
                .then(data => {
                    updateTimecodeDisplay();
                    updateDropFrameUI(); // Update UI when FPS changes
                })
                .catch(error => console.error('Error:', error));
        }
        
        function updateDropFrameUI() {
            const fps = document.getElementById('fpsSelect').value;
            const dropFrameCheckbox = document.getElementById('dropFrame');
            const dropFrameLabel = document.getElementById('dropFrameLabel');
            const dropFrameNote = document.getElementById('dropFrameNote');
            
            if (fps === '25') {
                // 25fps - disable drop frame and show note
                dropFrameCheckbox.checked = false;
                dropFrameCheckbox.disabled = true;
                dropFrameLabel.className = 'drop-frame-disabled';
                dropFrameNote.style.display = 'block';
                
                // Also update server to ensure drop frame is false for 25fps
                fetch('/config?dropFrame=false')
                    .then(response => response.json())
                    .then(data => console.log('Forced drop frame to false for 25fps'))
                    .catch(error => console.error('Error:', error));
            } else {
                // 24fps or 30fps - enable drop frame
                dropFrameCheckbox.disabled = false;
                dropFrameLabel.className = '';
                dropFrameNote.style.display = 'none';
            }
        }
        
        function updateDropFrame() {
            const dropFrame = document.getElementById('dropFrame').checked;
            const fps = document.getElementById('fpsSelect').value;
            
            // Don't allow drop frame for 25fps
            if (fps === '25') {
                document.getElementById('dropFrame').checked = false;
                return;
            }
            
            fetch('/config?dropFrame=' + (dropFrame ? 'true' : 'false'))
                .then(response => response.json())
                .then(data => updateTimecodeDisplay())
                .catch(error => console.error('Error:', error));
        }
        
        function toggleLTC(enable) {
            fetch('/ltc?enable=' + (enable ? 'true' : 'false'))
                .then(response => response.json())
                .then(data => updateTimecodeDisplay())
                .catch(error => console.error('Error:', error));
        }
        
        function setTimecode() {
            const hours = document.getElementById('hours').value;
            const minutes = document.getElementById('minutes').value;
            const seconds = document.getElementById('seconds').value;
            const frames = document.getElementById('frames').value;
            
            const url = `/setTimecode?hours=${hours}&minutes=${minutes}&seconds=${seconds}&frames=${frames}`;
            fetch(url)
                .then(response => response.json())
                .then(data => updateTimecodeDisplay())
                .catch(error => console.error('Error:', error));
        }

        // NEW: Set sync mode for all clients
        function setAllClientsSyncMode(mode) {
            fetch('/setSyncMode?mode=' + mode)
                .then(response => response.json())
                .then(data => {
                    console.log('Sync mode set response:', data);
                    // Update button states
                    updateSyncModeButtons(mode);
                })
                .catch(error => console.error('Error:', error));
        }

        // NEW: Update sync mode button states
        function updateSyncModeButtons(activeMode) {
            const buttons = document.querySelectorAll('.sync-mode-btn');
            buttons.forEach(btn => {
                btn.classList.remove('active');
                if (btn.textContent.includes(activeMode)) {
                    btn.classList.add('active');
                }
            });
        }
        
        function updateTimecodeDisplay() {
            fetch('/timecode')
                .then(response => response.json())
                .then(data => {
                    const timecodeStr = 
                        String(data.hours).padStart(2, '0') + ':' +
                        String(data.minutes).padStart(2, '0') + ':' +
                        String(data.seconds).padStart(2, '0') + ':' +
                        String(data.frames).padStart(2, '0');
                    document.getElementById('currentTimecode').textContent = timecodeStr;
                    
                    document.getElementById('fpsSelect').value = data.fps || 25;
                    
                    // Update drop frame UI based on current FPS
                    updateDropFrameUI();
                    
                    // Only set drop frame checkbox if not 25fps
                    if (data.fps !== 25) {
                        document.getElementById('dropFrame').checked = data.dropFrame || false;
                    } else {
                        document.getElementById('dropFrame').checked = false;
                    }
                    
                    const ltcEnabled = data.ltcEnabled !== undefined ? data.ltcEnabled : true;
                    const ltcStatus = document.getElementById('ltcStatus');
                    const ltcStatusCard = document.getElementById('ltcStatusCard');
                    const ltcFormat = document.getElementById('ltcFormat');
                    
                    if (ltcEnabled) {
                        ltcStatus.textContent = data.running ? 'Active - Running' : 'Active - Stopped (Silence)';
                        ltcStatus.style.color = data.running ? '#4CAF50' : '#ff9800';
                        ltcStatusCard.className = 'ltc-status ltc-enabled';
                    } else {
                        ltcStatus.textContent = 'Disabled';
                        ltcStatus.style.color = '#f44336';
                        ltcStatusCard.className = 'ltc-status ltc-disabled';
                    }
                    
                    const dropFrameSuffix = data.dropFrame ? ' DF' : '';
                    ltcFormat.textContent = data.fps + 'fps' + dropFrameSuffix + ' @ 48kHz';
                })
                .catch(error => console.error('Error:', error));
        }
        
        function updateClientsList() {
            fetch('/clients')
                .then(response => response.json())
                .then(data => {
                    const clientList = document.getElementById('clientList');
                    const clients = data.clients || [];
                    
                    if (clients.length === 0) {
                        clientList.innerHTML = '<div>No clients connected</div>';
                        return;
                    }
                    
                    clientList.innerHTML = clients.map(client => {
                        const drift = client.driftFrames || 0;
                        let driftClass = 'drift-good';
                        if (Math.abs(drift) > 2) driftClass = 'drift-warning';
                        if (Math.abs(drift) > 5) driftClass = 'drift-bad';
                        
                        // NEW: Show sync mode for each client
                        const syncMode = client.syncMode || 'AUTO';
                        let syncModeClass = 'sync-mode-auto';
                        let syncModeText = 'AUTO';
                        
                        if (syncMode === 'HARD') {
                            syncModeClass = 'sync-mode-hard';
                            syncModeText = 'HARD';
                        } else if (syncMode === 'SOFT') {
                            syncModeClass = 'sync-mode-soft';
                            syncModeText = 'SOFT';
                        }

                        // NEW: Individual client sync mode controls
                        const syncControls = `
                            <div style="margin-top: 8px;">
                                <small>Sync Mode: </small>
                                <button class="sync-mode-btn hard ${syncMode === 'HARD' ? 'active' : ''}" onclick="setClientSyncMode('${client.id}', 'HARD')" style="padding: 4px 8px; font-size: 0.8em;">HARD</button>
                                <button class="sync-mode-btn soft ${syncMode === 'SOFT' ? 'active' : ''}" onclick="setClientSyncMode('${client.id}', 'SOFT')" style="padding: 4px 8px; font-size: 0.8em;">SOFT</button>
                                <button class="sync-mode-btn auto ${syncMode === 'AUTO' ? 'active' : ''}" onclick="setClientSyncMode('${client.id}', 'AUTO')" style="padding: 4px 8px; font-size: 0.8em;">AUTO</button>
                            </div>
                        `;

                        return '<div class="client ' + (client.connected ? 'connected' : '') + '">' +
                            '<span class="status-led ' + (client.connected ? 'led-connected' : 'led-disconnected') + '"></span>' +
                            '<strong>' + client.name + '</strong> (' + client.id + ')<br>' +
                            '<small>IP: ' + client.ip + ' | ' +
                            'Status: ' + (client.connected ? 'Connected' : 'Disconnected') + 
                            ' | Sync: ' + (client.synced ? 'Synced' : 'Not Synced') +
                            ' | Mode: <span class="' + syncModeClass + '">' + syncModeText + '</span>' +
                            ' <span class="drift ' + driftClass + '">Drift: ' + drift + ' frames</span></small>' +
                            syncControls +
                            '</div>';
                    }).join('');
                })
                .catch(error => console.error('Error:', error));
        }

        // NEW: Set sync mode for individual client
        function setClientSyncMode(clientId, mode) {
            fetch('/setClientSyncMode?clientId=' + clientId + '&mode=' + mode)
                .then(response => response.json())
                .then(data => {
                    console.log('Client sync mode set response:', data);
                    updateClientsList(); // Refresh to show updated mode
                })
                .catch(error => console.error('Error:', error));
        }
        
        function updateNetworkStats() {
            fetch('/networkStats')
                .then(response => response.json())
                .then(data => {
                    const statsDiv = document.getElementById('networkStats');
                    statsDiv.innerHTML = 
                        'Packets Sent: ' + (data.packetsSent || 0) + '<br>' +
                        'Packets Received: ' + (data.packetsReceived || 0) + '<br>' +
                        'Packets Lost: ' + (data.packetsLost || 0) + '<br>' +
                        'Packet Loss: ' + (data.packetLossPercent || 0) + '%<br>' +
                        'Connected Clients: ' + (data.connectedClients || 0);
                })
                .catch(error => console.error('Error:', error));
        }
        
        // Update displays periodically
        setInterval(updateTimecodeDisplay, 100);
        setInterval(updateClientsList, 2000);
        setInterval(updateNetworkStats, 3000);
        
        // Initial load
        updateTimecodeDisplay();
        updateClientsList();
        updateNetworkStats();
    </script>
</body>
</html>
)=====";

void setup() {
  Serial.begin(115200);
  
  // Initialize hardware
  pinMode(SYNC_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  setupLTC();
  // Initialize preferences - MUST be before loadSettings()
  preferences.begin("timecode", false);
  loadSettings();
  
  // Initialize autosave timer
  lastAutoSave = millis();
  
  // Recalculate samples per bit after loading settings
  calculateSamplesPerBit();
  
  // Setup WiFi
  setupWiFi();
  
  // Setup UDP
  setupUDP();
  
  // Setup web server
  setupWebServer();
  
  // Start with loaded timecode state
  currentTimecode.startTime = millis();
  currentTimecode.lastFrameMicros = micros();
  
  Serial.println("=== Enhanced Timecode Master Started ===");
  Serial.printf("Resumed Timecode: %02d:%02d:%02d:%02d @ %dfps %s\n", 
                currentTimecode.hours, currentTimecode.minutes, 
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "DF" : "NDF");
  Serial.println("=== LTC SMPTE Audio Output ACTIVE on GPIO 25 ===");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupWiFi() {
  WiFi.softAP(ssid, password);
  Serial.println("WiFi Access Point Started");
}

void setupUDP() {
  udp.begin(UDP_PORT);
  Serial.println("UDP Server Started");
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", HTML_PAGE);
  });
  
  server.on("/timecode", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"hours\":" + String(currentTimecode.hours) + ",";
    json += "\"minutes\":" + String(currentTimecode.minutes) + ",";
    json += "\"seconds\":" + String(currentTimecode.seconds) + ",";
    json += "\"frames\":" + String(currentTimecode.frames) + ",";
    json += "\"fps\":" + String(currentTimecode.fps) + ",";
    json += "\"dropFrame\":" + String(currentTimecode.dropFrame ? "true" : "false") + ",";
    json += "\"running\":" + String(currentTimecode.running ? "true" : "false") + ",";
    json += "\"ltcEnabled\":" + String(ltcEnabled ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });
  
  server.on("/clients", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getClientsJSON());
  });
  
  server.on("/networkStats", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getNetworkStatsJSON());
  });
  
  server.on("/command", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("cmd")) {
      String cmd = request->getParam("cmd")->value();
      Serial.println("Command: " + cmd);
      
      if (cmd == "start") {
        currentTimecode.running = true;
        currentTimecode.startTime = millis();
        currentTimecode.lastFrameMicros = micros();
        // NEW: Immediately broadcast state change
        broadcastImmediateStateChange();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else if (cmd == "stop") {
        currentTimecode.running = false;
        // NEW: Immediately broadcast state change
        broadcastImmediateStateChange();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else if (cmd == "sync") {
        manualSyncAllClients();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else if (cmd == "reset") {
        currentTimecode.hours = 0;
        currentTimecode.minutes = 0;
        currentTimecode.seconds = 0;
        currentTimecode.frames = 0;
        currentTimecode.lastFrameMicros = micros();
        // NEW: Immediately broadcast state change
        broadcastImmediateStateChange();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      }
      
      saveSettings();
      buildLTCFrame();
    }
  });
  
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("fps")) {
      int newFps = request->getParam("fps")->value().toInt();
      
      // Only allow 24, 25, or 30 fps (removed 60fps)
      if (newFps == 24 || newFps == 25 || newFps == 30) {
        currentTimecode.fps = newFps;
        
        // Automatically disable drop frame for 25fps
        if (newFps == 25 && currentTimecode.dropFrame) {
          currentTimecode.dropFrame = false;
          Serial.println("Automatically disabled drop frame for 25fps");
        }
        
        calculateSamplesPerBit();
        saveSettings();
        broadcastTimecode();
        buildLTCFrame();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Invalid frame rate. Only 24, 25, or 30 fps allowed.\"}");
      }
    } else if (request->hasParam("dropFrame")) {
      bool newDropFrame = (request->getParam("dropFrame")->value() == "true");
      
      // Don't allow drop frame for 25fps - FIXED: Only log if actually changing
      if (currentTimecode.fps == 25) {
        if (newDropFrame && currentTimecode.dropFrame != false) {
          currentTimecode.dropFrame = false;
          Serial.println("Drop frame not allowed for 25fps - forcing to false");
        }
      } else {
        // Only update if FPS is not 25 and value is actually changing
        if (currentTimecode.dropFrame != newDropFrame) {
          currentTimecode.dropFrame = newDropFrame;
        }
      }
      
      saveSettings();
      broadcastTimecode();
      buildLTCFrame();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    }
  });
  
  // NEW: Set sync mode for all clients
  server.on("/setSyncMode", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("mode")) {
      String mode = request->getParam("mode")->value();
      Serial.printf("Setting sync mode for all clients: %s\n", mode.c_str());
      
      int clientsUpdated = 0;
      for (auto &client : clients) {
        if (client.connected) {
          // Send sync mode command to client
          String syncModePacket = "SYNC_MODE:" + mode;
          if (udp.beginPacket(client.ip, UDP_PORT)) {
            udp.write((const uint8_t*)syncModePacket.c_str(), syncModePacket.length());
            if (udp.endPacket()) {
              clientsUpdated++;
              Serial.printf("  Sent %s sync mode to %s (%s)\n", 
                            mode.c_str(), client.name.c_str(), client.ip.toString().c_str());
            }
          }
        }
      }
      
      String response = "{\"status\":\"ok\", \"clientsUpdated\":" + String(clientsUpdated) + "}";
      request->send(200, "application/json", response);
      Serial.printf("Sync mode %s sent to %d clients\n", mode.c_str(), clientsUpdated);
    } else {
      request->send(400, "application/json", "{\"error\":\"Missing mode parameter\"}");
    }
  });
  
  // NEW: Set sync mode for specific client
  server.on("/setClientSyncMode", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("clientId") && request->hasParam("mode")) {
      String clientId = request->getParam("clientId")->value();
      String mode = request->getParam("mode")->value();
      
      Serial.printf("Setting sync mode for client %s: %s\n", clientId.c_str(), mode.c_str());
      
      bool clientFound = false;
      for (auto &client : clients) {
        if (client.id == clientId && client.connected) {
          // Send sync mode command to specific client
          String syncModePacket = "SYNC_MODE:" + mode;
          if (udp.beginPacket(client.ip, UDP_PORT)) {
            udp.write((const uint8_t*)syncModePacket.c_str(), syncModePacket.length());
            if (udp.endPacket()) {
              clientFound = true;
              Serial.printf("  Sent %s sync mode to %s (%s)\n", 
                            mode.c_str(), client.name.c_str(), client.ip.toString().c_str());
            }
          }
          break;
        }
      }
      
      if (clientFound) {
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(404, "application/json", "{\"error\":\"Client not found or not connected\"}");
      }
    } else {
      request->send(400, "application/json", "{\"error\":\"Missing parameters\"}");
    }
  });
  
 server.on("/ltc", HTTP_GET, [](AsyncWebServerRequest *request){
  if (request->hasParam("enable")) {
    String enableStr = request->getParam("enable")->value();
    bool newLtcState = (enableStr == "true");
    
    Serial.printf("Web LTC control: %s\n", newLtcState ? "ENABLE" : "DISABLE");
    
    // Update master's LTC state
    ltcEnabled = newLtcState;
    preferences.putBool("ltcEnabled", ltcEnabled);
    
    // Send LTC command to all clients
    String ltcPacket = "LTC:" + String(ltcEnabled ? "1" : "0");
    Serial.printf("Sending LTC packet to clients: %s\n", ltcPacket.c_str());
    
    int clientsSent = 0;
    for (auto &client : clients) {
      if (client.connected) {
        if (udp.beginPacket(client.ip, UDP_PORT)) {
          udp.write((const uint8_t*)ltcPacket.c_str(), ltcPacket.length());
          if (udp.endPacket()) {
            clientsSent++;
            Serial.printf("  Sent to %s (%s)\n", client.name.c_str(), client.ip.toString().c_str());
          } else {
            Serial.printf("  Failed to send to %s\n", client.name.c_str());
          }
        }
      }
    }
    
    Serial.printf("LTC command sent to %d clients\n", clientsSent);
    
    String response = "{\"status\":\"ok\", \"ltcEnabled\":" + String(ltcEnabled ? "true" : "false") + "}";
    request->send(200, "application/json", response);
  } else {
    request->send(400, "application/json", "{\"error\":\"Missing enable parameter\"}");
  }
});
  
  server.on("/setTimecode", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("hours") && request->hasParam("minutes") && 
        request->hasParam("seconds") && request->hasParam("frames")) {
      
      currentTimecode.hours = request->getParam("hours")->value().toInt();
      currentTimecode.minutes = request->getParam("minutes")->value().toInt();
      currentTimecode.seconds = request->getParam("seconds")->value().toInt();
      currentTimecode.frames = request->getParam("frames")->value().toInt();
      currentTimecode.lastFrameMicros = micros();
      
      saveSettings();
      // NEW: Immediately broadcast the new timecode
      broadcastImmediateStateChange();
      buildLTCFrame();
      
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    }
  });
  
  server.begin();
}

void loadSettings() {
  currentTimecode.fps = preferences.getUInt("fps", 25);
  currentTimecode.dropFrame = preferences.getBool("dropFrame", false);
  
  // Ensure 25fps doesn't have drop frame
  if (currentTimecode.fps == 25 && currentTimecode.dropFrame) {
    currentTimecode.dropFrame = false;
    Serial.println("Ensured drop frame is false for 25fps");
  }
  
  currentTimecode.hours = preferences.getUInt("hours", 0);
  currentTimecode.minutes = preferences.getUInt("minutes", 0);
  currentTimecode.seconds = preferences.getUInt("seconds", 0);
  currentTimecode.frames = preferences.getUInt("frames", 0);
  currentTimecode.running = preferences.getBool("running", true);
  ltcEnabled = preferences.getBool("ltcEnabled", true);
  
  // Load the last autosave timestamp
  unsigned long lastSaveTime = preferences.getULong("lastSaveTime", 0);
  unsigned long currentTime = millis();
  
  // Calculate how much time has passed since last autosave
  if (lastSaveTime > 0) {
    // Estimate time passed (in milliseconds) since last save
    // This is approximate since we don't know exact power-off time
    unsigned long estimatedTimePassed = AUTO_SAVE_INTERVAL + 5000; // Add buffer (autosave interval + 5 seconds)
    
    Serial.printf("Last autosave was at: %lu ms\n", lastSaveTime);
    Serial.printf("Estimated time passed since last save: %lu ms\n", estimatedTimePassed);
    
    // Convert time passed to frames and advance timecode
    advanceTimecodeByMillis(estimatedTimePassed);
    
    Serial.printf("Adjusted timecode after restart: %02d:%02d:%02d:%02d\n",
                  currentTimecode.hours, currentTimecode.minutes,
                  currentTimecode.seconds, currentTimecode.frames);
  }
  
  currentTimecode.lastFrameMicros = micros();
  currentTimecode.startTime = millis();
  
  Serial.printf("Master RESUMED timecode: %02d:%02d:%02d:%02d @ %dfps %s\n", 
                currentTimecode.hours, currentTimecode.minutes, 
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "DF" : "NDF");
}

void saveSettings() {
  preferences.putUInt("fps", currentTimecode.fps);
  
  // FIXED: Only save drop frame if it's actually different
  bool currentDropFrame = currentTimecode.dropFrame;
  if (currentTimecode.fps == 25) {
    currentDropFrame = false; // Force false for 25fps
  }
  preferences.putBool("dropFrame", currentDropFrame);
  
  preferences.putUInt("hours", currentTimecode.hours);
  preferences.putUInt("minutes", currentTimecode.minutes);
  preferences.putUInt("seconds", currentTimecode.seconds);
  preferences.putUInt("frames", currentTimecode.frames);
  preferences.putBool("running", currentTimecode.running);
  preferences.putBool("ltcEnabled", ltcEnabled);
  
  // Also save the current time for offset calculation on restart
  preferences.putULong("lastSaveTime", millis());
  
  Serial.printf("Master saved timecode: %02d:%02d:%02d:%02d (Timestamp: %lu)\n",
                currentTimecode.hours, currentTimecode.minutes,
                currentTimecode.seconds, currentTimecode.frames, millis());
}

// Enhanced timecode update with drift compensation
void updateTimecode() {
  if (!currentTimecode.running) return;
  
  unsigned long currentMicros = micros();
  unsigned long frameInterval = 1000000 / currentTimecode.fps;
  
  if (currentMicros - currentTimecode.lastFrameMicros >= frameInterval) {
    currentTimecode.lastFrameMicros += frameInterval;
    
    currentTimecode.frames++;
    packetSequence++;
    
    // Handle drop frame counting for 30fps only (not for 25fps)
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
  }
}

void broadcastTimecode() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastBroadcast >= FRAME_BROADCAST_INTERVAL) {
    lastBroadcast = currentTime;
    
    String packet = "TC:";
    packet += String(currentTimecode.hours) + ":";
    packet += String(currentTimecode.minutes) + ":";
    packet += String(currentTimecode.seconds) + ":";
    packet += String(currentTimecode.frames) + ":";
    packet += String(currentTimecode.fps) + ":";
    packet += String(currentTimecode.dropFrame ? "1" : "0") + ":";
    packet += String(currentTimecode.running ? "1" : "0") + ":";
    packet += String(packetSequence) + ":";
    packet += String(millis());
    
    udp.beginPacket(broadcastIP, UDP_PORT);
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    udp.endPacket();
    
    updateNetworkStats(true, false);
  }
}

void manualSyncAllClients() {
  String syncPacket = "SYNC:";
  syncPacket += String(currentTimecode.hours) + ":";
  syncPacket += String(currentTimecode.minutes) + ":";
  syncPacket += String(currentTimecode.seconds) + ":";
  syncPacket += String(currentTimecode.frames) + ":";
  syncPacket += String(currentTimecode.fps) + ":";
  syncPacket += String(currentTimecode.dropFrame ? "1" : "0") + ":";
  syncPacket += String(currentTimecode.running ? "1" : "0") + ":";
  syncPacket += String(millis());
  
  Serial.printf("Manual sync broadcast: %s\n", syncPacket.c_str());
  
  // Send to broadcast and individually to each client
  for (int i = 0; i < 3; i++) { // Increased from 2 to 3 for better reliability
    udp.beginPacket(broadcastIP, UDP_PORT);
    udp.write((const uint8_t*)syncPacket.c_str(), syncPacket.length());
    udp.endPacket();
    
    for (auto &client : clients) {
      if (client.connected) {
        udp.beginPacket(client.ip, UDP_PORT);
        udp.write((const uint8_t*)syncPacket.c_str(), syncPacket.length());
        udp.endPacket();
      }
    }
    delay(10); // Reduced delay for faster transmission
  }
  
  for (auto &client : clients) {
    if (client.connected) {
      client.synced = true;
    }
  }
  
  Serial.println("Manual sync completed");
}

void processUDPPackets() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char packetBuffer[255];
    int len = udp.read(packetBuffer, 255);
    if (len > 0) {
      packetBuffer[len] = 0;
      
      String packet = String(packetBuffer);
      IPAddress clientIP = udp.remoteIP();
      
      // FIXED: Only update connection for known clients, don't create unknown ones here
      updateClientConnection(clientIP, true);
      
      Serial.printf("Received packet from %s: %s\n", clientIP.toString().c_str(), packet.c_str());
      
      if (packet.startsWith("REG:")) {
        processRegistration(packet, clientIP);
      } else if (packet.startsWith("STATUS:")) {
        processStatusUpdate(packet, clientIP);
      } else if (packet.startsWith("HEARTBEAT:")) {
        processHeartbeat(packet, clientIP);
      } else if (packet.startsWith("SYNC_REQUEST:")) {
        processSyncRequest(packet, clientIP);
      } else if (packet.startsWith("SYNC_MODE_ACK:")) {
        processSyncModeACK(packet, clientIP);
      } else if (packet.startsWith("LTC_ACK:")) {
        processLTCACK(packet, clientIP);
      }
      // Don't create unknown clients for other packet types
    }
  }
}
void processLTCACK(String packet, IPAddress clientIP) {
  int firstColon = packet.indexOf(':');
  int secondColon = packet.indexOf(':', firstColon + 1);
  
  if (firstColon != -1 && secondColon != -1) {
    String clientId = packet.substring(firstColon + 1, secondColon);
    String ltcState = packet.substring(secondColon + 1);
    
    for (auto &client : clients) {
      if (client.id == clientId) {
        client.lastSeen = millis();
        client.connected = true;
        Serial.printf("Client %s LTC ACK: %s\n", client.name.c_str(), ltcState.c_str());
        break;
      }
    }
  }
}
void processSyncModeACK(String packet, IPAddress clientIP) {
    int firstColon = packet.indexOf(':');
    int secondColon = packet.indexOf(':', firstColon + 1);
    
    if (firstColon != -1 && secondColon != -1) {
        String clientId = packet.substring(firstColon + 1, secondColon);
        String syncMode = packet.substring(secondColon + 1);
        
        for (auto &client : clients) {
            if (client.id == clientId) {
                client.syncMode = syncMode;
                client.lastSeen = millis();
                Serial.printf("Client %s confirmed sync mode: %s\n", 
                              client.name.c_str(), syncMode.c_str());
                break;
            }
        }
    }
}

// Add this new function to update client connection status
void updateClientConnection(IPAddress clientIP, bool connected) {
  for (auto &client : clients) {
    if (client.ip == clientIP) {
      bool wasConnected = client.connected;
      client.connected = connected;
      client.lastSeen = millis();
      
      if (connected && !wasConnected) {
        Serial.printf("Client CONNECTED via packet: %s (%s) from %s\n", 
                      client.name.c_str(), client.id.c_str(), clientIP.toString().c_str());
      }
      break;
    }
  }
  // REMOVED: Don't create unknown clients here - wait for registration
}


void processRegistration(String packet, IPAddress clientIP) {
  int firstColon = packet.indexOf(':');
  int secondColon = packet.indexOf(':', firstColon + 1);
  
  if (firstColon != -1 && secondColon != -1) {
    String clientId = packet.substring(firstColon + 1, secondColon);
    String clientName = packet.substring(secondColon + 1);
    
    bool clientExists = false;
    int existingIndex = -1;
    
    // First, check if we already have this client ID (regardless of IP)
    for (size_t i = 0; i < clients.size(); i++) {
      if (clients[i].id == clientId) {
        clientExists = true;
        existingIndex = i;
        break;
      }
    }
    
    if (clientExists) {
      // Update existing client
      ClientInfo &client = clients[existingIndex];
      
      // Check if IP has changed
      if (client.ip != clientIP) {
        Serial.printf("Client IP changed: %s (%s) from %s to %s\n", 
                      client.name.c_str(), client.id.c_str(), 
                      client.ip.toString().c_str(), clientIP.toString().c_str());
      }
      
      client.ip = clientIP;
      client.connected = true;
      client.lastSeen = millis();
      client.name = clientName; // Update name in case it changed
      
      Serial.printf("Client re-registered: %s (%s) from %s\n", 
                    clientName.c_str(), clientId.c_str(), clientIP.toString().c_str());
    } else {
      // Check if we have an unknown client with this IP
      bool unknownClientFound = false;
      for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].ip == clientIP && clients[i].id.startsWith("UNKNOWN_")) {
          // Upgrade unknown client to known client
          clients[i].id = clientId;
          clients[i].name = clientName;
          clients[i].connected = true;
          clients[i].lastSeen = millis();
          unknownClientFound = true;
          
          Serial.printf("Upgraded unknown client to: %s (%s) from %s\n", 
                        clientName.c_str(), clientId.c_str(), clientIP.toString().c_str());
          break;
        }
      }
      
      if (!unknownClientFound) {
        // Create new client
        ClientInfo newClient;
        newClient.id = clientId;
        newClient.name = clientName;
        newClient.ip = clientIP;
        newClient.connected = true;
        newClient.synced = false;
        newClient.driftFrames = 0;
        newClient.lastSeen = millis();
        newClient.lastSequence = 0;
        newClient.lastResponse = millis();
        newClient.syncMode = "AUTO";
        clients.push_back(newClient);
        
        Serial.printf("NEW client registered: %s (%s) from %s\n", 
                      clientName.c_str(), clientId.c_str(), clientIP.toString().c_str());
      }
    }
    
    // Always send ACK and sync to registered clients
    String ackPacket = "ACK:" + clientId;
    udp.beginPacket(clientIP, UDP_PORT);
    udp.write((const uint8_t*)ackPacket.c_str(), ackPacket.length());
    udp.endPacket();
    
    // Send sync to ensure client gets current timecode
    sendDirectSync(clientIP);
    
    // Clean up any duplicate clients (same IP but different IDs)
    cleanupDuplicateClients();
    
    // Print updated client list
    Serial.printf("Current clients: %d\n", clients.size());
    for (const auto &client : clients) {
      Serial.printf("  - %s (%s) IP: %s - Connected: %s\n", 
                    client.name.c_str(), client.id.c_str(), 
                    client.ip.toString().c_str(),
                    client.connected ? "YES" : "NO");
    }
  }
}
void cleanupDuplicateClients() {
  for (size_t i = 0; i < clients.size(); i++) {
    for (size_t j = i + 1; j < clients.size(); j++) {
      // If same IP but different IDs, remove the unknown one
      if (clients[i].ip == clients[j].ip && clients[i].id != clients[j].id) {
        if (clients[i].id.startsWith("UNKNOWN_")) {
          Serial.printf("Removing duplicate unknown client: %s (%s)\n", 
                        clients[i].name.c_str(), clients[i].id.c_str());
          clients.erase(clients.begin() + i);
          i--; // Adjust index after removal
          break;
        } else if (clients[j].id.startsWith("UNKNOWN_")) {
          Serial.printf("Removing duplicate unknown client: %s (%s)\n", 
                        clients[j].name.c_str(), clients[j].id.c_str());
          clients.erase(clients.begin() + j);
          j--; // Adjust index after removal
        }
      }
    }
  }
}
void processStatusUpdate(String packet, IPAddress clientIP) {
  int firstColon = packet.indexOf(':');
  int secondColon = packet.indexOf(':', firstColon + 1);
  int thirdColon = packet.indexOf(':', secondColon + 1);
  
  if (firstColon != -1 && secondColon != -1 && thirdColon != -1) {
    String clientId = packet.substring(firstColon + 1, secondColon);
    int drift = packet.substring(secondColon + 1, thirdColon).toInt();
    uint32_t sequence = packet.substring(thirdColon + 1).toInt();
    
    for (auto &client : clients) {
      if (client.id == clientId) {
        client.driftFrames = drift;
        client.lastSequence = sequence;
        client.lastSeen = millis();
        client.lastResponse = millis();
        client.connected = true;
        break;
      }
    }
  }
}

void processHeartbeat(String packet, IPAddress clientIP) {
  int firstColon = packet.indexOf(':');
  if (firstColon != -1) {
    String clientId = packet.substring(firstColon + 1);
    
    for (auto &client : clients) {
      if (client.id == clientId) {
        client.lastSeen = millis();
        client.connected = true;
        break;
      }
    }
  }
}

void processSyncRequest(String packet, IPAddress clientIP) {
  int firstColon = packet.indexOf(':');
  if (firstColon != -1) {
    String clientId = packet.substring(firstColon + 1);
    
    for (auto &client : clients) {
      if (client.id == clientId) {
        Serial.printf("Sending sync to reconnected client: %s\n", client.name.c_str());
        sendDirectSync(clientIP);
        break;
      }
    }
  }
}

void processAck(String packet, IPAddress clientIP) {
  // Handle acknowledgments if needed
  // Currently just updates lastSeen
  int firstColon = packet.indexOf(':');
  if (firstColon != -1) {
    String clientId = packet.substring(firstColon + 1);
    
    for (auto &client : clients) {
      if (client.id == clientId) {
        client.lastSeen = millis();
        break;
      }
    }
  }
}

void sendDirectSync(IPAddress clientIP) {
  String syncPacket = "SYNC:";
  syncPacket += String(currentTimecode.hours) + ":";
  syncPacket += String(currentTimecode.minutes) + ":";
  syncPacket += String(currentTimecode.seconds) + ":";
  syncPacket += String(currentTimecode.frames) + ":";
  syncPacket += String(currentTimecode.fps) + ":";
  syncPacket += String(currentTimecode.dropFrame ? "1" : "0") + ":";
  syncPacket += String(currentTimecode.running ? "1" : "0") + ":";
  syncPacket += String(millis());
  
  udp.beginPacket(clientIP, UDP_PORT);
  udp.write((const uint8_t*)syncPacket.c_str(), syncPacket.length());
  udp.endPacket();
  
  for (auto &client : clients) {
    if (client.ip == clientIP) {
      client.synced = true;
      break;
    }
  }
  
  Serial.printf("Sent direct sync to %s\n", clientIP.toString().c_str());
}

void checkClientTimeouts() {
  unsigned long currentTime = millis();
  for (auto &client : clients) {
    if (client.connected && (currentTime - client.lastSeen > CLIENT_TIMEOUT)) {
      client.connected = false;
      client.synced = false;
      Serial.printf("Client TIMEOUT: %s (%s) - Last seen %lu ms ago\n", 
                    client.name.c_str(), client.id.c_str(), 
                    (currentTime - client.lastSeen));
    }
  }
}


void cleanupDisconnectedClients() {
  unsigned long currentTime = millis();
  if (currentTime - lastClientCleanup > CLIENT_CLEANUP_INTERVAL) {
    lastClientCleanup = currentTime;
    
    clients.erase(
      std::remove_if(clients.begin(), clients.end(),
          [](const ClientInfo& client) {
            // Remove unknown clients faster (5 minutes) and known clients slower (24 hours)
            if (client.id.startsWith("UNKNOWN_")) {
              return !client.connected && (millis() - client.lastSeen > 5 * 60 * 1000); // 5 minutes
            } else {
              return !client.connected && (millis() - client.lastSeen > 24 * 60 * 60 * 1000); // 24 hours
            }
          }),
      clients.end()
    );
    
    Serial.printf("Client cleanup completed. %d clients in list.\n", clients.size());
  }
}

void checkSyncButton() {
  bool currentButtonState = digitalRead(SYNC_BUTTON_PIN);
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    manualSyncAllClients();
    delay(50);
  }
  lastButtonState = currentButtonState;
}

void updateStatusLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  unsigned long currentTime = millis();
  unsigned long interval = 1000;
  
  if (!clients.empty()) {
    bool allSynced = true;
    for (const auto &client : clients) {
      if (client.connected && (!client.synced || abs(client.driftFrames) > 2)) {
        allSynced = false;
        break;
      }
    }
    
    if (allSynced) {
      digitalWrite(STATUS_LED_PIN, HIGH);
      return;
    } else {
      interval = 200;
    }
  } else {
    interval = 2000;
  }
  
  if (currentTime - lastBlink > interval) {
    lastBlink = currentTime;
    ledState = !ledState;
    digitalWrite(STATUS_LED_PIN, ledState);
  }
}

String getClientsJSON() {
  String json = "{\"clients\":[";
  for (size_t i = 0; i < clients.size(); i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"id\":\"" + clients[i].id + "\",";
    json += "\"name\":\"" + clients[i].name + "\",";
    json += "\"ip\":\"" + clients[i].ip.toString() + "\",";
    json += "\"connected\":" + String(clients[i].connected ? "true" : "false") + ",";
    json += "\"synced\":" + String(clients[i].synced ? "true" : "false") + ",";
    json += "\"driftFrames\":" + String(clients[i].driftFrames) + ",";
    // NEW: Add sync mode to client JSON (default to AUTO)
    json += "\"syncMode\":\"" + (clients[i].syncMode.length() > 0 ? clients[i].syncMode : "AUTO") + "\"";
    json += "}";
  }
  json += "]}";
  return json;
}

String getNetworkStatsJSON() {
  int connectedClients = 0;
  for (const auto &client : clients) {
    if (client.connected) connectedClients++;
  }
  
  float packetLossPercent = 0;
  if (networkStats.packetsSent > 0) {
    packetLossPercent = (float)networkStats.packetsLost / networkStats.packetsSent * 100;
  }
  
  String json = "{";
  json += "\"packetsSent\":" + String(networkStats.packetsSent) + ",";
  json += "\"packetsReceived\":" + String(networkStats.packetsReceived) + ",";
  json += "\"packetsLost\":" + String(networkStats.packetsLost) + ",";
  json += "\"packetLossPercent\":" + String(packetLossPercent, 2) + ",";
  json += "\"connectedClients\":" + String(connectedClients);
  json += "}";
  return json;
}

void updateNetworkStats(bool packetSent, bool packetReceived) {
  if (packetSent) networkStats.packetsSent++;
  if (packetReceived) networkStats.packetsReceived++;
  
  // Simple packet loss estimation (for demonstration)
  if (millis() - networkStats.lastStatsUpdate > 5000) {
    networkStats.lastStatsUpdate = millis();
    if (networkStats.packetsSent > networkStats.packetsReceived + 10) {
      networkStats.packetsLost += 1;
    }
  }
}

void loop() {
  updateTimecode();
  broadcastTimecode();
  processUDPPackets();
  checkClientTimeouts(); // Check more frequently
  checkSyncButton();
  updateStatusLED();
    cleanupDisconnectedClients();
     static unsigned long lastDuplicateCheck = 0;
  if (millis() - lastDuplicateCheck > 10000) { // Every 10 seconds
    lastDuplicateCheck = millis();
    cleanupDuplicateClients();
  }
  // More frequent client status checks
  static unsigned long lastClientCheck = 0;
  if (millis() - lastClientCheck > 500) { // Check every 500ms
    lastClientCheck = millis();
    checkClientTimeouts();
  }
  
  unsigned long currentMillis = millis();
  if (currentMillis - lastAutoSave >= AUTO_SAVE_INTERVAL) {
    saveSettings();
    lastAutoSave = currentMillis;
  }
  
  // More frequent debug output
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 1000) { // Every 1 second instead of 2
    lastDebug = millis();
    int connectedCount = 0;
    for (const auto &client : clients) {
      if (client.connected) connectedCount++;
    }
    
    Serial.printf("MASTER: Timecode: %02d:%02d:%02d:%02d | Clients: %d/%d connected\n", 
                  currentTimecode.hours, currentTimecode.minutes, 
                  currentTimecode.seconds, currentTimecode.frames,
                  connectedCount, clients.size());
  }
  
  delay(1);
}