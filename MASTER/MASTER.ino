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

// LTC variables - MAKE THESE VOLATILE FOR ISR
volatile uint8_t frameBits[BITS_PER_FRAME];
volatile int bitIndex = 0;
volatile int sampleIndex = 0;
volatile bool currentLevel = false;
volatile bool ltcEnabled = true;
volatile int samplesPerBit = 0;

// Timecode structure for LTC generation - ALL VOLATILE
struct LTC_Timecode {
  uint8_t hours;
  uint8_t minutes;
   uint8_t seconds;
   uint8_t frames;
   uint8_t fps;
   bool dropFrame;
   bool running;
   unsigned long lastFrameMicros;
   uint32_t totalFrames;
};

volatile LTC_Timecode ltcTimecode = {0, 0, 0, 0, 25, false, true, 0, 0};

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

// Main Timecode structure (for networking and UI)
struct Timecode {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
  uint8_t frames;
  uint8_t fps;
  bool dropFrame;
  bool running;
  uint32_t sequence;
  unsigned long lastStableUpdate;
  uint32_t totalFrames;
};

Timecode currentTimecode = {0, 0, 0, 0, 25, false, true, 0, 0, 0};

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
  uint16_t packetLoss;
  unsigned long lastSyncTime; // Track when client was last synced
};

std::vector<ClientInfo> clients;
const unsigned long CLIENT_TIMEOUT = 3000;
const unsigned long CLIENT_CLEANUP_INTERVAL = 60000;

// Enhanced timing constants
const unsigned long FRAME_BROADCAST_INTERVAL = 40;
const unsigned long HEARTBEAT_INTERVAL = 5000;

// Physical pins
const int SYNC_BUTTON_PIN = 0;
const int STATUS_LED_PIN = 2;
bool lastButtonState = HIGH;

// Enhanced packet management
uint32_t packetSequence = 0;
unsigned long lastBroadcast = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastClientCleanup = 0;
unsigned long lastAutoSave = 0;
const unsigned long AUTO_SAVE_INTERVAL = 5000;

// Resync configuration - CONFIGURABLE
unsigned long resyncInterval = 300000; // Default: 5 minutes (300 seconds)
const unsigned long MIN_RESYNC_INTERVAL = 10000;   // 10 seconds minimum
const unsigned long MAX_RESYNC_INTERVAL = 3600000; // 1 hour maximum

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
void broadcastTimecode();
void manualSyncAllClients();
void processUDPPackets();
void processRegistration(String packet, IPAddress clientIP);
void processStatusUpdate(String packet, IPAddress clientIP);
void processHeartbeat(String packet, IPAddress clientIP);
void processSyncRequest(String packet, IPAddress clientIP);
void processLTCACK(String packet, IPAddress clientIP);
void sendDirectSync(IPAddress clientIP);
void checkClientTimeouts();
void cleanupDisconnectedClients();
void cleanupDuplicateClients();
void checkSyncButton();
void updateStatusLED();
String getClientsJSON();
String getNetworkStatsJSON();
String getConfigJSON();
void updateNetworkStats(bool packetSent, bool packetReceived);
void advanceTimecodeByMillis(unsigned long millisToAdvance);
void broadcastImmediateStateChange();
void sendStateToAllClients();
void syncLTCWithCurrentTimecode();
void getCurrentTimecodeFromLTC();
void updateClientConnection(IPAddress clientIP, bool connected);
void checkAutoResync();
void performResyncIfNeeded(ClientInfo &client);

// Enhanced audio output with better levels
inline void dacWriteLevel(bool level) {
  dacWrite(OUTPUT_PIN, level ? 132 : 124);
}

void calculateSamplesPerBit() {
  samplesPerBit = SAMPLE_RATE / (ltcTimecode.fps * BITS_PER_FRAME);
  Serial.printf("Master: Samples per bit: %d for %d fps\n", samplesPerBit, ltcTimecode.fps);
}

// Build LTC frame from volatile LTC timecode
void buildLTCFrame() {
  // Use local copies of the current timecode
  uint8_t hours = currentTimecode.hours;
  uint8_t minutes = currentTimecode.minutes;
  uint8_t seconds = currentTimecode.seconds;
  uint8_t frames = currentTimecode.frames;
  bool dropFrame = currentTimecode.dropFrame;
  
  // Replace memset with manual clearing for volatile array
  for (int i = 0; i < BITS_PER_FRAME; i++) {
    frameBits[i] = 0;
  }
  
  uint8_t frameUnits = frames % 10;
  uint8_t frameTens = frames / 10;
  uint8_t secondUnits = seconds % 10;
  uint8_t secondTens = seconds / 10;
  uint8_t minuteUnits = minutes % 10;
  uint8_t minuteTens = minutes / 10;
  uint8_t hourUnits = hours % 10;
  uint8_t hourTens = hours / 10;
  
  // Frame units (bits 0-3)
  frameBits[0] = (frameUnits >> 0) & 1;
  frameBits[1] = (frameUnits >> 1) & 1;
  frameBits[2] = (frameUnits >> 2) & 1;
  frameBits[3] = (frameUnits >> 3) & 1;
  
  // Frame tens (bits 8-9)
  frameBits[8] = (frameTens >> 0) & 1;
  frameBits[9] = (frameTens >> 1) & 1;
  
  // Drop frame (bit 10)
  frameBits[10] = dropFrame ? 1 : 0;
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

// Rock-solid LTC timer - completely self-contained
void IRAM_ATTR onLtcTimer() {
  portENTER_CRITICAL_ISR(&ltcTimerMux);
  
  if (ltcTimecode.running && ltcEnabled) {
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
        
        // Advance frame in LTC timecode - THIS IS THE KEY FIX!
        ltcTimecode.frames++;
        ltcTimecode.totalFrames++;
        
        // Handle frame rollover with strict boundaries
        if (ltcTimecode.frames >= ltcTimecode.fps) {
          ltcTimecode.frames = 0;
          ltcTimecode.seconds++;
          
          if (ltcTimecode.seconds >= 60) {
            ltcTimecode.seconds = 0;
            ltcTimecode.minutes++;
            
            if (ltcTimecode.minutes >= 60) {
              ltcTimecode.minutes = 0;
              ltcTimecode.hours++;
              
              if (ltcTimecode.hours >= 24) {
                ltcTimecode.hours = 0;
              }
            }
          }
          
          // Drop frame logic for 30fps only
          if (ltcTimecode.dropFrame && ltcTimecode.fps == 30) {
            if (ltcTimecode.minutes % 10 != 0 && ltcTimecode.seconds == 0 && ltcTimecode.frames < 2) {
              ltcTimecode.frames = 2;
            }
          }
        }
        
        // Rebuild LTC frame with new timecode
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

// Sync main timecode with LTC timecode
void syncLTCWithCurrentTimecode() {
  portENTER_CRITICAL(&ltcTimerMux);
  ltcTimecode.hours = currentTimecode.hours;
  ltcTimecode.minutes = currentTimecode.minutes;
  ltcTimecode.seconds = currentTimecode.seconds;
  ltcTimecode.frames = currentTimecode.frames;
  ltcTimecode.fps = currentTimecode.fps;
  ltcTimecode.dropFrame = currentTimecode.dropFrame;
  ltcTimecode.running = currentTimecode.running;
  ltcTimecode.totalFrames = currentTimecode.totalFrames;
  
  // Rebuild LTC frame with new timecode
  buildLTCFrame();
  bitIndex = 0;
  sampleIndex = 0;
  portEXIT_CRITICAL(&ltcTimerMux);
}

// Get current timecode from LTC (for broadcasting)
void getCurrentTimecodeFromLTC() {
  portENTER_CRITICAL(&ltcTimerMux);
  currentTimecode.hours = ltcTimecode.hours;
  currentTimecode.minutes = ltcTimecode.minutes;
  currentTimecode.seconds = ltcTimecode.seconds;
  currentTimecode.frames = ltcTimecode.frames;
  currentTimecode.totalFrames = ltcTimecode.totalFrames;
  portEXIT_CRITICAL(&ltcTimerMux);
}

void advanceOneFrame() {
  currentTimecode.frames++;
  
  // STRICT frame boundary checking
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
    
    // Drop frame logic for 24fps and 30fps (but NOT for 25fps)
    if (currentTimecode.dropFrame) {
      if (currentTimecode.fps == 30) {
        // Drop frame for 30fps (NTSC)
        if (currentTimecode.minutes % 10 != 0 && currentTimecode.seconds == 0) {
          currentTimecode.frames = 2; // Skip frames 0 and 1
        }
      } else if (currentTimecode.fps == 24) {
        // Drop frame for 24fps (Film - 23.976fps)
        // For 24fps drop frame, we drop frames differently
        // This is a simplified implementation for 23.976fps
        if (currentTimecode.minutes % 10 != 0 && currentTimecode.seconds == 0) {
          currentTimecode.frames = 1; // Skip frame 0 for 24fps drop frame
        }
      }
      // No drop frame logic for 25fps
    }
  }
  
  currentTimecode.totalFrames++;
}

void advanceTimecodeByMillis(unsigned long millisToAdvance) {
  if (millisToAdvance == 0) return;
  
  unsigned long framesToAdvance = (millisToAdvance * currentTimecode.fps) / 1000;
  
  for (unsigned long i = 0; i < framesToAdvance; i++) {
    advanceOneFrame();
  }
  
  // Sync LTC with the new timecode
  syncLTCWithCurrentTimecode();
}

void broadcastImmediateStateChange() {
  getCurrentTimecodeFromLTC(); // Get the actual timecode from LTC generator
  
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
  
  for (int i = 0; i < 3; i++) {
    udp.beginPacket(broadcastIP, UDP_PORT);
    udp.write((const uint8_t*)packet.c_str(), packet.length());
    udp.endPacket();
    delay(5);
  }
}

const char HTML_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <title>Timecode Master - Hard Sync Only</title>
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
        .config-section { background: #fff3cd; padding: 15px; border-radius: 5px; margin: 10px 0; border-left: 4px solid #ffc107; }
        .config-input { width: 120px; padding: 8px; margin: 5px; border: 1px solid #ccc; border-radius: 4px; }
        .config-label { font-weight: bold; margin-right: 10px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Timecode Master - Hard Sync Only</h1>
        
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
                <div id="dropFrameNote" style="font-size: 0.9em; color: #666;">
                    Note: Drop frame is only applicable to 30fps (29.97fps)
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

        <div class="config-section">
            <h2>Resync Configuration</h2>
            <div class="config-group">
                <span class="config-label">Resync Interval (minutes):</span>
                <input type="number" id="resyncInterval" min="1" max="60" value="5" class="config-input">
                <button onclick="updateResyncInterval()">Update</button>
            </div>
            <p style="font-size: 0.9em; margin-top: 10px;">
                <strong>Note:</strong> Clients will only resync when reconnecting to master or when this interval elapses.<br>
                Resync occurs only if drift is more than 3 frames. Default: 5 minutes.
            </p>
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
    
    // Immediately update the UI to show the selected value
    document.getElementById('fpsSelect').value = fps;
    
    fetch('/config?fps=' + fps)
        .then(response => response.json())
        .then(data => {
            if (data.status === 'ok') {
                // Success - the server accepted the FPS change
                console.log('FPS updated successfully to: ' + fps);
                updateDropFrameUI();
                
                // If switching to non-30fps, ensure drop frame is disabled
                if (fps !== '30') {
                    document.getElementById('dropFrame').checked = false;
                }
            } else {
                // Server rejected the change, revert to actual value
                updateTimecodeDisplay();
            }
        })
        .catch(error => {
            console.error('Error updating FPS:', error);
            // On error, refresh display to show actual value
            updateTimecodeDisplay();
        });
}
        
        function updateDropFrameUI() {
    const fps = document.getElementById('fpsSelect').value;
    const dropFrameCheckbox = document.getElementById('dropFrame');
    const dropFrameLabel = document.getElementById('dropFrameLabel');
    const dropFrameNote = document.getElementById('dropFrameNote');
    
    // Only disable drop frame for 25fps (PAL)
    if (fps === '25') {
        dropFrameCheckbox.checked = false;
        dropFrameCheckbox.disabled = true;
        dropFrameLabel.className = 'drop-frame-disabled';
        dropFrameNote.style.display = 'block';
        dropFrameNote.textContent = 'Note: Drop frame is not applicable to 25fps (PAL)';
    } else {
        dropFrameCheckbox.disabled = false;
        dropFrameLabel.className = '';
        dropFrameNote.style.display = 'block';
        
        if (fps === '30') {
            dropFrameNote.textContent = 'Note: Drop frame creates 29.97fps timecode (NTSC)';
        } else {
            dropFrameNote.textContent = 'Note: Drop frame creates 23.976fps timecode (Film)';
        }
    }
}
        
        function updateDropFrame() {
    const dropFrameCheckbox = document.getElementById('dropFrame');
    const fps = document.getElementById('fpsSelect').value;
    
    // Only process if NOT 25fps (24fps and 30fps are allowed to use drop frame)
    if (fps !== '25') {
        const dropFrame = dropFrameCheckbox.checked;
        
        fetch('/config?dropFrame=' + (dropFrame ? 'true' : 'false'))
            .then(response => response.json())
            .then(data => {
                if (data.status === 'ok') {
                    console.log('Drop frame updated to: ' + dropFrame + ' for ' + fps + 'fps');
                } else {
                    // Revert checkbox if server rejected
                    updateTimecodeDisplay();
                }
            })
            .catch(error => {
                console.error('Error updating drop frame:', error);
                updateTimecodeDisplay();
            });
    } else {
        // Force uncheck and disable for 25fps
        dropFrameCheckbox.checked = false;
        dropFrameCheckbox.disabled = true;
    }
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

        function updateResyncInterval() {
            const interval = document.getElementById('resyncInterval').value;
            fetch('/config?resyncInterval=' + interval)
                .then(response => response.json())
                .then(data => {
                    console.log('Resync interval updated:', data);
                    alert('Resync interval updated to ' + interval + ' minutes');
                })
                .catch(error => console.error('Error:', error));
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
            
            // CRITICAL FIX: Only update FPS dropdown if it's different from current selection
            const currentFpsSelect = document.getElementById('fpsSelect');
            if (currentFpsSelect.value !== String(data.fps)) {
                currentFpsSelect.value = String(data.fps || 25);
            }
            
            updateDropFrameUI();
            
            // Handle drop frame checkbox - enable for 24fps and 30fps, disable only for 25fps
            const dropFrameCheckbox = document.getElementById('dropFrame');
            if (data.fps === 25) {
                dropFrameCheckbox.checked = false;
                dropFrameCheckbox.disabled = true;
            } else {
                dropFrameCheckbox.checked = data.dropFrame || false;
                dropFrameCheckbox.disabled = false;
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
                        
                        return '<div class="client ' + (client.connected ? 'connected' : '') + '">' +
                            '<span class="status-led ' + (client.connected ? 'led-connected' : 'led-disconnected') + '"></span>' +
                            '<strong>' + client.name + '</strong> (' + client.id + ')<br>' +
                            '<small>IP: ' + client.ip + ' | ' +
                            'Status: ' + (client.connected ? 'Connected' : 'Disconnected') + 
                            ' | Sync: ' + (client.synced ? 'Synced' : 'Not Synced') +
                            ' <span class="drift ' + driftClass + '">Drift: ' + drift + ' frames</span></small>' +
                            '</div>';
                    }).join('');
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

        function loadConfig() {
            fetch('/config')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('resyncInterval').value = data.resyncInterval || 5;
                })
                .catch(error => console.error('Error:', error));
        }
        
        // Initialize on page load
        document.addEventListener('DOMContentLoaded', function() {
            updateTimecodeDisplay();
            updateClientsList();
            updateNetworkStats();
            loadConfig();
        });
        
        setInterval(updateTimecodeDisplay, 100);
        setInterval(updateClientsList, 2000);
        setInterval(updateNetworkStats, 3000);
    </script>
</body>
</html>
)=====";

void setup() {
  Serial.begin(115200);
  
  // Initialize hardware
  pinMode(SYNC_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  
  // Initialize preferences
  preferences.begin("timecode", false);
  loadSettings();
  
  // Initialize autosave timer
  lastAutoSave = millis();
  
  // Setup LTC FIRST - this is critical
  setupLTC();
  
  // Recalculate samples per bit after loading settings
  calculateSamplesPerBit();
  
  // Setup WiFi
  setupWiFi();
  
  // Setup UDP
  setupUDP();
  
  // Setup web server
  setupWebServer();
  
  // Sync LTC with loaded timecode
  syncLTCWithCurrentTimecode();
  
  Serial.println("=== Enhanced Timecode Master Started ===");
  Serial.printf("Resumed Timecode: %02d:%02d:%02d:%02d @ %dfps %s\n", 
                currentTimecode.hours, currentTimecode.minutes, 
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "DF" : "NDF");
  Serial.printf("Resync Interval: %lu minutes\n", resyncInterval / 60000);
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
    getCurrentTimecodeFromLTC(); // Get actual timecode from LTC generator
    
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

server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("fps")) {
        int newFps = request->getParam("fps")->value().toInt();
        
        if (newFps == 24 || newFps == 25 || newFps == 30) {
            // Only proceed if FPS is actually changing
            if (currentTimecode.fps != newFps) {
                currentTimecode.fps = newFps;
                
                // CRITICAL: Only allow drop frame for 24fps and 30fps, NOT for 25fps
                if (newFps == 25 && currentTimecode.dropFrame) {
                    currentTimecode.dropFrame = false;
                    Serial.printf("Automatically disabled drop frame for %dfps (PAL)\n", newFps);
                }
                // For 24fps and 30fps, keep the current drop frame setting
                
                // Update LTC generator with new FPS
                syncLTCWithCurrentTimecode();
                
                // Recalculate samples per bit for LTC generation
                calculateSamplesPerBit();
                
                saveSettings();
                broadcastImmediateStateChange();
                
                Serial.printf("FPS changed to: %d, Drop Frame: %s\n", 
                    newFps, currentTimecode.dropFrame ? "true" : "false");
            }
            
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(400, "application/json", "{\"error\":\"Invalid frame rate. Only 24, 25, or 30 fps allowed.\"}");
        }
    } else if (request->hasParam("dropFrame")) {
        bool newDropFrame = (request->getParam("dropFrame")->value() == "true");
        
        // CRITICAL: Only allow drop frame for 24fps and 30fps, NOT for 25fps
        if (currentTimecode.fps == 25) {
            // Force drop frame to false if 25fps
            if (currentTimecode.dropFrame != false) {
                currentTimecode.dropFrame = false;
                Serial.println("Drop frame not allowed for 25fps (PAL) - forcing to false");
            }
            request->send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Drop frame disabled for 25fps (PAL)\"}");
        } else {
            // For 24fps and 30fps, allow drop frame changes
            if (currentTimecode.dropFrame != newDropFrame) {
                currentTimecode.dropFrame = newDropFrame;
                Serial.printf("Drop frame %s for %dfps\n", 
                    newDropFrame ? "enabled" : "disabled", currentTimecode.fps);
                
                syncLTCWithCurrentTimecode();
                saveSettings();
                broadcastImmediateStateChange();
            }
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        }
    } else if (request->hasParam("resyncInterval")) {
        unsigned long newInterval = request->getParam("resyncInterval")->value().toInt() * 60000; // Convert minutes to ms
        
        if (newInterval >= MIN_RESYNC_INTERVAL && newInterval <= MAX_RESYNC_INTERVAL) {
            resyncInterval = newInterval;
            preferences.putULong("resyncInterval", resyncInterval);
            request->send(200, "application/json", "{\"status\":\"ok\", \"resyncInterval\":" + String(resyncInterval / 60000) + "}");
            Serial.printf("Resync interval updated to: %lu minutes\n", resyncInterval / 60000);
        } else {
            request->send(400, "application/json", "{\"error\":\"Resync interval must be between " + 
                         String(MIN_RESYNC_INTERVAL / 60000) + " and " + String(MAX_RESYNC_INTERVAL / 60000) + " minutes\"}");
        }
    }
});
  
  server.on("/command", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("cmd")) {
      String cmd = request->getParam("cmd")->value();
      Serial.println("Command: " + cmd);
      
      if (cmd == "start") {
        currentTimecode.running = true;
        syncLTCWithCurrentTimecode();
        broadcastImmediateStateChange();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else if (cmd == "stop") {
        currentTimecode.running = false;
        syncLTCWithCurrentTimecode();
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
        currentTimecode.totalFrames = 0;
        syncLTCWithCurrentTimecode();
        broadcastImmediateStateChange();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      }
      
      saveSettings();
    }
  });
  
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("fps")) {
      int newFps = request->getParam("fps")->value().toInt();
      
      if (newFps == 24 || newFps == 25 || newFps == 30) {
        currentTimecode.fps = newFps;
        
        // Only allow drop frame for 30fps
        if (newFps != 30 && currentTimecode.dropFrame) {
          currentTimecode.dropFrame = false;
          Serial.printf("Automatically disabled drop frame for %dfps\n", newFps);
        }
        
        calculateSamplesPerBit();
        saveSettings();
        syncLTCWithCurrentTimecode();
        broadcastImmediateStateChange();
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Invalid frame rate. Only 24, 25, or 30 fps allowed.\"}");
      }
    } else if (request->hasParam("dropFrame")) {
      bool newDropFrame = (request->getParam("dropFrame")->value() == "true");
      
      // Only allow drop frame for 30fps
      if (currentTimecode.fps != 30) {
        if (newDropFrame && currentTimecode.dropFrame != false) {
          currentTimecode.dropFrame = false;
          Serial.println("Drop frame not allowed for this frame rate - forcing to false");
        }
      } else {
        if (currentTimecode.dropFrame != newDropFrame) {
          currentTimecode.dropFrame = newDropFrame;
        }
      }
      
      saveSettings();
      syncLTCWithCurrentTimecode();
      broadcastImmediateStateChange();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    } else if (request->hasParam("resyncInterval")) {
      unsigned long newInterval = request->getParam("resyncInterval")->value().toInt() * 60000; // Convert minutes to ms
      
      if (newInterval >= MIN_RESYNC_INTERVAL && newInterval <= MAX_RESYNC_INTERVAL) {
        resyncInterval = newInterval;
        preferences.putULong("resyncInterval", resyncInterval);
        request->send(200, "application/json", "{\"status\":\"ok\", \"resyncInterval\":" + String(resyncInterval / 60000) + "}");
        Serial.printf("Resync interval updated to: %lu minutes\n", resyncInterval / 60000);
      } else {
        request->send(400, "application/json", "{\"error\":\"Resync interval must be between " + 
                     String(MIN_RESYNC_INTERVAL / 60000) + " and " + String(MAX_RESYNC_INTERVAL / 60000) + " minutes\"}");
      }
    }
  });
  
 server.on("/ltc", HTTP_GET, [](AsyncWebServerRequest *request){
  if (request->hasParam("enable")) {
    String enableStr = request->getParam("enable")->value();
    bool newLtcState = (enableStr == "true");
    
    Serial.printf("Web LTC control: %s\n", newLtcState ? "ENABLE" : "DISABLE");
    
    ltcEnabled = newLtcState;
    preferences.putBool("ltcEnabled", ltcEnabled);
    
    String ltcPacket = "LTC:" + String(ltcEnabled ? "1" : "0");
    Serial.printf("Sending LTC packet to clients: %s\n", ltcPacket.c_str());
    
    int clientsSent = 0;
    for (auto &client : clients) {
      if (client.connected) {
        if (udp.beginPacket(client.ip, UDP_PORT)) {
          udp.write((const uint8_t*)ltcPacket.c_str(), ltcPacket.length());
          if (udp.endPacket()) {
            clientsSent++;
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
    
    // Get and validate timecode values
    uint8_t newHours = request->getParam("hours")->value().toInt();
    uint8_t newMinutes = request->getParam("minutes")->value().toInt();
    uint8_t newSeconds = request->getParam("seconds")->value().toInt();
    uint8_t newFrames = request->getParam("frames")->value().toInt();
    
    // Validate timecode ranges
    if (newHours >= 0 && newHours <= 23 &&
        newMinutes >= 0 && newMinutes <= 59 &&
        newSeconds >= 0 && newSeconds <= 59 &&
        newFrames >= 0 && newFrames < currentTimecode.fps) {
      
      // CRITICAL: Stop LTC generation during update
      portENTER_CRITICAL(&ltcTimerMux);
      
      // Update the main timecode structure
      currentTimecode.hours = newHours;
      currentTimecode.minutes = newMinutes;
      currentTimecode.seconds = newSeconds;
      currentTimecode.frames = newFrames;
      
      // Also update the LTC timecode structure to match
      ltcTimecode.hours = newHours;
      ltcTimecode.minutes = newMinutes;
      ltcTimecode.seconds = newSeconds;
      ltcTimecode.frames = newFrames;
      
      // Reset LTC generator state to start fresh
      bitIndex = 0;
      sampleIndex = 0;
      currentLevel = false;
      
      // Rebuild LTC frame with new timecode
      buildLTCFrame();
      
      portEXIT_CRITICAL(&ltcTimerMux);
      
      Serial.printf("Webpage timecode set: %02d:%02d:%02d:%02d @ %dfps\n",
                    currentTimecode.hours, currentTimecode.minutes,
                    currentTimecode.seconds, currentTimecode.frames,
                    currentTimecode.fps);
      
      // Save to preferences
      saveSettings();
      
      // Force immediate broadcast to all clients
      broadcastTimecode();
      
      // Send sync packet to all clients to force them to update
      manualSyncAllClients();
      
      request->send(200, "application/json", "{\"status\":\"ok\", \"message\":\"Timecode updated successfully\"}");
    } else {
      // Invalid timecode values
      String errorMsg = "{\"error\":\"Invalid timecode values. Hours: 0-23, Minutes: 0-59, Seconds: 0-59, Frames: 0-\" + String(currentTimecode.fps - 1) + \"\"}";
      request->send(400, "application/json", errorMsg);
      Serial.println("Invalid timecode values received from webpage");
    }
  } else {
    request->send(400, "application/json", "{\"error\":\"Missing timecode parameters\"}");
  }
});
  
  server.begin();
}
void debugTimecodeState() {
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 5000) { // Every 5 seconds
    lastDebug = millis();
    Serial.printf("DEBUG Timecode State: %02d:%02d:%02d:%02d @ %dfps | LTC bitIndex: %d, sampleIndex: %d\n",
                  currentTimecode.hours, currentTimecode.minutes,
                  currentTimecode.seconds, currentTimecode.frames,
                  currentTimecode.fps, bitIndex, sampleIndex);
  }
}
void loadSettings() {
  currentTimecode.fps = preferences.getUInt("fps", 25);
  currentTimecode.dropFrame = preferences.getBool("dropFrame", false);
  
  // Only allow drop frame for 30fps
  if (currentTimecode.fps != 30 && currentTimecode.dropFrame) {
    currentTimecode.dropFrame = false;
    Serial.printf("Ensured drop frame is false for %dfps\n", currentTimecode.fps);
  }
  
  currentTimecode.hours = preferences.getUInt("hours", 0);
  currentTimecode.minutes = preferences.getUInt("minutes", 0);
  currentTimecode.seconds = preferences.getUInt("seconds", 0);
  currentTimecode.frames = preferences.getUInt("frames", 0);
  currentTimecode.running = preferences.getBool("running", true);
  ltcEnabled = preferences.getBool("ltcEnabled", true);
  
  // Initialize totalFrames based on loaded timecode
  currentTimecode.totalFrames = ((currentTimecode.hours * 3600) + 
                                (currentTimecode.minutes * 60) + 
                                currentTimecode.seconds) * currentTimecode.fps + 
                                currentTimecode.frames;
  
  // Load resync interval (default: 5 minutes)
  resyncInterval = preferences.getULong("resyncInterval", 300000);
  
  unsigned long lastSaveTime = preferences.getULong("lastSaveTime", 0);
  unsigned long currentTime = millis();
  
  if (lastSaveTime > 0) {
    unsigned long estimatedTimePassed = AUTO_SAVE_INTERVAL + 5000;
    
    Serial.printf("Last autosave was at: %lu ms\n", lastSaveTime);
    Serial.printf("Estimated time passed since last save: %lu ms\n", estimatedTimePassed);
    
    advanceTimecodeByMillis(estimatedTimePassed);
    
    Serial.printf("Adjusted timecode after restart: %02d:%02d:%02d:%02d\n",
                  currentTimecode.hours, currentTimecode.minutes,
                  currentTimecode.seconds, currentTimecode.frames);
  }
  
  currentTimecode.lastStableUpdate = micros();
  
  Serial.printf("Master RESUMED timecode: %02d:%02d:%02d:%02d @ %dfps %s\n", 
                currentTimecode.hours, currentTimecode.minutes, 
                currentTimecode.seconds, currentTimecode.frames,
                currentTimecode.fps,
                currentTimecode.dropFrame ? "DF" : "NDF");
  Serial.printf("Resync Interval: %lu minutes\n", resyncInterval / 60000);
}

void saveSettings() {
  getCurrentTimecodeFromLTC(); // Save the actual LTC timecode
  
  preferences.putUInt("fps", currentTimecode.fps);
  
  bool currentDropFrame = currentTimecode.dropFrame;
  // Only save drop frame for 30fps
  if (currentTimecode.fps != 30) {
    currentDropFrame = false;
  }
  preferences.putBool("dropFrame", currentDropFrame);
  
  preferences.putUInt("hours", currentTimecode.hours);
  preferences.putUInt("minutes", currentTimecode.minutes);
  preferences.putUInt("seconds", currentTimecode.seconds);
  preferences.putUInt("frames", currentTimecode.frames);
  preferences.putBool("running", currentTimecode.running);
  preferences.putBool("ltcEnabled", ltcEnabled);
  preferences.putULong("lastSaveTime", millis());
  preferences.putULong("resyncInterval", resyncInterval);
}

void broadcastTimecode() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastBroadcast >= FRAME_BROADCAST_INTERVAL) {
    lastBroadcast = currentTime;
    
    getCurrentTimecodeFromLTC(); // Get actual timecode from LTC generator
    
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
  getCurrentTimecodeFromLTC(); // Get actual timecode from LTC generator
  
  String syncPacket = "SYNC:";
  syncPacket += String(currentTimecode.hours) + ":";
  syncPacket += String(currentTimecode.minutes) + ":";
  syncPacket += String(currentTimecode.seconds) + ":";
  syncPacket += String(currentTimecode.frames) + ":";
  syncPacket += String(currentTimecode.fps) + ":";
  syncPacket += String(currentTimecode.dropFrame ? "1" : "0") + ":";
  syncPacket += String(currentTimecode.running ? "1" : "0") + ":";
  syncPacket += String(millis());
  
  for (int i = 0; i < 3; i++) {
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
    delay(10);
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
      
      updateClientConnection(clientIP, true);
      
      if (packet.startsWith("REG:")) {
        processRegistration(packet, clientIP);
      } else if (packet.startsWith("STATUS:")) {
        processStatusUpdate(packet, clientIP);
      } else if (packet.startsWith("HEARTBEAT:")) {
        processHeartbeat(packet, clientIP);
      } else if (packet.startsWith("SYNC_REQUEST:")) {
        processSyncRequest(packet, clientIP);
      } else if (packet.startsWith("LTC_ACK:")) {
        processLTCACK(packet, clientIP);
      }
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

void updateClientConnection(IPAddress clientIP, bool connected) {
  for (auto &client : clients) {
    if (client.ip == clientIP) {
      bool wasConnected = client.connected;
      client.connected = connected;
      client.lastSeen = millis();
      
      if (connected && !wasConnected) {
        Serial.printf("Client CONNECTED via packet: %s (%s) from %s\n", 
                      client.name.c_str(), client.id.c_str(), clientIP.toString().c_str());
        // Check if we need to resync this newly connected client
        performResyncIfNeeded(client);
      }
      break;
    }
  }
}

void processRegistration(String packet, IPAddress clientIP) {
  int firstColon = packet.indexOf(':');
  int secondColon = packet.indexOf(':', firstColon + 1);
  
  if (firstColon != -1 && secondColon != -1) {
    String clientId = packet.substring(firstColon + 1, secondColon);
    String clientName = packet.substring(secondColon + 1);
    
    bool clientExists = false;
    int existingIndex = -1;
    
    for (size_t i = 0; i < clients.size(); i++) {
      if (clients[i].id == clientId) {
        clientExists = true;
        existingIndex = i;
        break;
      }
    }
    
    if (clientExists) {
      ClientInfo &client = clients[existingIndex];
      
      if (client.ip != clientIP) {
        Serial.printf("Client IP changed: %s (%s) from %s to %s\n", 
                      client.name.c_str(), client.id.c_str(), 
                      client.ip.toString().c_str(), clientIP.toString().c_str());
      }
      
      client.ip = clientIP;
      client.connected = true;
      client.lastSeen = millis();
      client.name = clientName;
      
      Serial.printf("Client re-registered: %s (%s) from %s\n", 
                    clientName.c_str(), clientId.c_str(), clientIP.toString().c_str());
      
      // Check if we need to resync this reconnected client
      performResyncIfNeeded(client);
    } else {
      bool unknownClientFound = false;
      for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].ip == clientIP && clients[i].id.startsWith("UNKNOWN_")) {
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
        newClient.lastSyncTime = 0; // Never synced
        clients.push_back(newClient);
        
        Serial.printf("NEW client registered: %s (%s) from %s\n", 
                      clientName.c_str(), clientId.c_str(), clientIP.toString().c_str());
        
        // Send immediate sync to new client
        sendDirectSync(clientIP);
      }
    }
    
    String ackPacket = "ACK:" + clientId;
    udp.beginPacket(clientIP, UDP_PORT);
    udp.write((const uint8_t*)ackPacket.c_str(), ackPacket.length());
    udp.endPacket();
    
    sendDirectSync(clientIP);
    
    cleanupDuplicateClients();
    
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
      if (clients[i].ip == clients[j].ip && clients[i].id != clients[j].id) {
        if (clients[i].id.startsWith("UNKNOWN_")) {
          Serial.printf("Removing duplicate unknown client: %s (%s)\n", 
                        clients[i].name.c_str(), clients[i].id.c_str());
          clients.erase(clients.begin() + i);
          i--;
          break;
        } else if (clients[j].id.startsWith("UNKNOWN_")) {
          Serial.printf("Removing duplicate unknown client: %s (%s)\n", 
                        clients[j].name.c_str(), clients[j].id.c_str());
          clients.erase(clients.begin() + j);
          j--;
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
        
        // Check if we need to resync based on drift and time
        performResyncIfNeeded(client);
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

void sendDirectSync(IPAddress clientIP) {
  getCurrentTimecodeFromLTC(); // Get actual timecode from LTC generator
  
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
      client.lastSyncTime = millis();
      client.driftFrames = 0;
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
            if (client.id.startsWith("UNKNOWN_")) {
              return !client.connected && (millis() - client.lastSeen > 5 * 60 * 1000);
            } else {
              return !client.connected && (millis() - client.lastSeen > 24 * 60 * 60 * 1000);
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
    json += "\"driftFrames\":" + String(clients[i].driftFrames);
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

String getConfigJSON() {
  String json = "{";
  json += "\"resyncInterval\":" + String(resyncInterval / 60000);
  json += "}";
  return json;
}

void updateNetworkStats(bool packetSent, bool packetReceived) {
  if (packetSent) networkStats.packetsSent++;
  if (packetReceived) networkStats.packetsReceived++;
  
  if (millis() - networkStats.lastStatsUpdate > 5000) {
    networkStats.lastStatsUpdate = millis();
    if (networkStats.packetsSent > networkStats.packetsReceived + 10) {
      networkStats.packetsLost += 1;
    }
  }
}

// NEW: Check if a client needs resync based on drift and time
void performResyncIfNeeded(ClientInfo &client) {
  if (!client.connected) return;
  
  unsigned long currentTime = millis();
  bool needsResync = false;
  
  // Check if drift is more than 3 frames
  if (abs(client.driftFrames) > 3) {
    // Check if enough time has passed since last sync
    if (currentTime - client.lastSyncTime > resyncInterval) {
      needsResync = true;
      Serial.printf("Client %s needs resync: drift=%d frames, last sync=%lu ms ago\n",
                    client.name.c_str(), client.driftFrames, currentTime - client.lastSyncTime);
    }
  }
  
  // If client was never synced, sync now
  if (client.lastSyncTime == 0) {
    needsResync = true;
    Serial.printf("Client %s never synced - performing initial sync\n", client.name.c_str());
  }
  
  if (needsResync) {
    sendDirectSync(client.ip);
  }
}

// NEW: Periodically check all connected clients for resync
void checkAutoResync() {
  static unsigned long lastResyncCheck = 0;
  unsigned long currentTime = millis();
  
  // Check every 30 seconds
  if (currentTime - lastResyncCheck < 30000) return;
  lastResyncCheck = currentTime;
  
  for (auto &client : clients) {
    if (client.connected) {
      performResyncIfNeeded(client);
    }
  }
}

void loop() {
  // LTC generation is completely handled by hardware timer - NO UPDATES NEEDED
  
  // Network operations
  broadcastTimecode();
  processUDPPackets();
  checkClientTimeouts();
  checkSyncButton();
  updateStatusLED();
  cleanupDisconnectedClients();
  checkAutoResync(); // NEW: Check for auto resync
  debugTimecodeState();
  // More frequent client status checks
  static unsigned long lastClientCheck = 0;
  if (millis() - lastClientCheck > 500) {
    lastClientCheck = millis();
    checkClientTimeouts();
  }
  
  // Auto-save
  unsigned long currentMillis = millis();
  if (currentMillis - lastAutoSave >= AUTO_SAVE_INTERVAL) {
    saveSettings();
    lastAutoSave = currentMillis;
  }
  
  // Debug output
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 1000) {
    lastDebug = millis();
    
    getCurrentTimecodeFromLTC(); // Get actual timecode for display
    
    int connectedCount = 0;
    int syncedCount = 0;
    for (const auto &client : clients) {
      if (client.connected) {
        connectedCount++;
        if (client.synced && abs(client.driftFrames) <= 3) {
          syncedCount++;
        }
      }
    }
    
    Serial.printf("MASTER: Timecode: %02d:%02d:%02d:%02d | Clients: %d/%d connected, %d synced | Resync: %lu min\n", 
                  currentTimecode.hours, currentTimecode.minutes, 
                  currentTimecode.seconds, currentTimecode.frames,
                  connectedCount, clients.size(), syncedCount,
                  resyncInterval / 60000);
  }
  
  delay(1);
}