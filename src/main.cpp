/*
---------------------------------------------------------------------------------
KXPA100 Remote Control via IC-705 & M5 Stack (Multicore Version)
---------------------------------------------------------------------------------

Author: 15.08.2025, René Mederlet, HB9HGW

Description:
This software enables remote control of the Elecraft KXPA100 HF amplifier via
an ICOM IC-705 transceiver.

Architecture overview:
1. Core 1 (UI Task): Handles Button inputs and Display updates. High responsiveness.
2. Core 0 (Backend Task): Handles WiFi (CAT) and Serial (KXPA) communication.
   Data is exchanged via Mutex-protected shared variables.


---------------------------------------------------------------------------------
*/

#include <M5Unified.h>
#include "KXPA100Controller.h"
#include "CatWifiClient.h"
#include "Secrets.h"

#define TITLE_VERSION   "KXPA100 Control"
#define MIN_POS         0      
#define MAX_POS         10     

// Serial Configuration
#define RX_PIN          16
#define TX_PIN          17
#define BAUD_RATE       38400  
#define DELAY_COMM_MS   20     
#define INVERTED        true   
#define BAUD_DEBUG      115200 

// Timing Constants
#define DISPLAY_UPDATE_MS       500
#define POWEROFF_TIMEOUT_MS     30000  // 30 seconds
#define POWEROFF_WARNING_MS     25000  // Warn at 25 seconds
#define BACKEND_POLL_MS         200    // Reduced from 50ms - saves power
#define MUTEX_TIMEOUT_MS        50
#define SEMAPHORE_TIMEOUT_TICKS pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)

// Sprite Dimensions
#define IMG0_WIDTH      320
#define IMG0_HEIGHT     30
#define IMG1_WIDTH      160
#define IMG1_HEIGHT     180
#define IMG1a_WIDTH     160
#define IMG1a_HEIGHT    180
#define IMG2_WIDTH      320
#define IMG2_HEIGHT     30

// CAT Configuration
const unsigned long CAT_TIMEOUT_MS = 10000; 
const char* CAT_COMMAND = "f\n";

// Layout Constants
const int LINE1_Y       = 15;
const int LINE2_Y       = LINE1_Y + 55;
const int LINE3_Y       = LINE2_Y + 35;
const int LINE4_Y       = LINE3_Y + 35;
const int LINE_LEFT_X   = 5;
const int VALUES_X      = 105;

// Button Repeat Configuration
const unsigned long BTN_REPEAT_DELAY_INITIAL_MS = 400;
const unsigned long BTN_REPEAT_RATE_MS = 150;

// -----------------------------------------------------------------------------------------
// SHARED DATA STRUCT (Protected by Mutex)
// -----------------------------------------------------------------------------------------
struct SharedData {
  int bandIndex = 0;
  String bandName = "";
  String power = "0";
  String temp = "0";
  String swr = "1.0";
  String antenna = "";
  String mode = "";
  String faults = "";
  String voltage = "";
  
  bool catConnected = false;
  bool kxpaConnected = false;
  
  // Manual band change request
  volatile bool manualChangeReq = false; 
  int manualTargetBand = 0;
  
  // Dirty flags for optimized rendering
  bool bandDirty = true;
  bool powerDirty = true;
  bool tempDirty = true;
  bool swrDirty = true;
  bool antennaDirty = true;
  bool modeDirty = true;
  bool faultsDirty = true;
  bool voltageDirty = true;
  bool connectionDirty = true;
};

SharedData sharedState;
SemaphoreHandle_t dataMutex;

// Global objects
M5Canvas img0(&M5.Lcd);
M5Canvas img1(&M5.Lcd);
M5Canvas img1a(&M5.Lcd);
M5Canvas img2(&M5.Lcd);

KXPA100Controller kxpa(Serial2, RX_PIN, TX_PIN, BAUD_RATE, DELAY_COMM_MS, INVERTED);
CatWifiClient cat(ssid, password, CAT_SERVER, RIGCTLD_PORT, CAT_TIMEOUT_MS);

// UI Local Variables
int uiBandCounter = 0;
bool uiUpdatingBand = false;
unsigned long timerDisplay = 0;
unsigned long timerLastKxpaConnection = 0;
bool powerOffWarningShown = false;

// Button Repeat Logic
unsigned long btnRepeatTimer = 0;

// -----------------------------------------------------------------------------------------
// PROTOTYPES
// -----------------------------------------------------------------------------------------
void backendTask(void * pvParameters);
void drawLeftSprite(const String& band, const String& power, const String& temp, const String& swr, bool catConn);
void drawRightSprite(const String& antRaw, const String& mode, const String& faults, const String& voltage);
void showStatusLine(const String& text, int color);
void showPowerOffWarning();
bool checkStringChanged(const String& newVal, const String& oldVal);

// -----------------------------------------------------------------------------------------
// SETUP
// -----------------------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Power.begin();
  
  Serial.begin(BAUD_DEBUG);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Booting...");

  // Create Mutex for thread safety
  dataMutex = xSemaphoreCreateMutex();
  if (dataMutex == NULL) {
    Serial.println("FATAL: Failed to create mutex");
    while(1) delay(1000); // Halt
  }

  // Initialize Hardware
  kxpa.begin();
  cat.begin();

  // Initialize Sprites
  img0.setColorDepth(8); 
  img0.createSprite(IMG0_WIDTH, IMG0_HEIGHT); 
  img0.fillSprite(WHITE);
  
  img1.setColorDepth(8); 
  img1.setTextSize(1); 
  img1.createSprite(IMG1_WIDTH, IMG1_HEIGHT); 
  img1.fillSprite(WHITE);
  
  img1a.setColorDepth(8); 
  img1a.createSprite(IMG1a_WIDTH, IMG1a_HEIGHT); 
  img1a.fillSprite(WHITE);
  
  img2.setColorDepth(8); 
  img2.setFont(&fonts::FreeSans12pt7b); 
  img2.createSprite(IMG2_WIDTH, IMG2_HEIGHT);
  
  // Initial Push
  img0.pushSprite(0, 0);
  img1.pushSprite(0, 30);
  img1a.pushSprite(160, 30);
  img2.pushSprite(0, 211);
  
  // Initial wait for KXPA (with timeout)
  Serial.println("Waiting for KXPA100...");
  unsigned long startWait = millis();
  while (!kxpa.checkConnection() && millis() - startWait < 5000) { 
    delay(100); 
  }
  
  if (kxpa.checkConnection()) {
    Serial.println("KXPA100 connected");
    // Initial Settings
    kxpa.setBand(5);  // Start at 20m
    kxpa.setMode("^MDA;");  // Automatic mode
  } else {
    Serial.println("KXPA100 not detected - will retry");
  }
  
  // Start Backend Task on Core 0
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    backendTask,   // Function
    "BackendTask", // Name
    8192,          // Stack size
    NULL,          // Params
    1,             // Priority
    NULL,          // Handle
    0              // Core ID (0)
  );
  
  if (taskCreated != pdPASS) {
    Serial.println("FATAL: Failed to create backend task");
    while(1) delay(1000); // Halt
  }

  timerDisplay = millis();
  timerLastKxpaConnection = millis();
}

// -----------------------------------------------------------------------------------------
// BACKEND TASK (Core 0)
// Handles WiFi, CAT, and Serial Comm
// -----------------------------------------------------------------------------------------
void backendTask(void * pvParameters) {
  int currentBandIdx = 0;
  unsigned long lastPoll = 0;

  while (true) {
    unsigned long now = millis();
    
    // Update CAT client state machine (non-blocking)
    cat.update();
    
    // Poll at reduced rate to save power
    if (now - lastPoll < BACKEND_POLL_MS) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    lastPoll = now;
    
    // 1. Handle Manual Band Change Request from UI
    bool manualReq = false;
    int target = 0;
    
    if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
      if (sharedState.manualChangeReq) {
        manualReq = true;
        target = sharedState.manualTargetBand;
        sharedState.manualChangeReq = false;
      }
      xSemaphoreGive(dataMutex);
    }

    // Local vars for this iteration - pre-reserve to avoid fragmentation
    String p_bandName, p_power, p_temp, p_swr;
    String p_ant, p_mode, p_fault, p_volt;
    p_bandName.reserve(8);
    p_power.reserve(8);
    p_temp.reserve(8);
    p_swr.reserve(8);
    p_ant.reserve(16);
    p_mode.reserve(16);
    p_fault.reserve(16);
    p_volt.reserve(8);
    
    bool justSwitched = false;

    if (manualReq) {
      kxpa.setBand(target);
      currentBandIdx = target;
      p_bandName = kxpa.getBandName(target);
      justSwitched = true; 
    }

    // 2. Poll KXPA Status
    bool kxpaOk = kxpa.checkConnection();
    
    if (kxpaOk) {
      if (!justSwitched) {
        int newBand = kxpa.getBand();
        if (newBand >= 0) {  // Check for error
          currentBandIdx = newBand;
          p_bandName = kxpa.getBandName(currentBandIdx);
        }
      }
      
      // Read other values
      p_power = kxpa.getPower();
      p_temp  = kxpa.getTemperature();
      p_swr   = kxpa.getSWR();
      p_ant   = kxpa.getAntenna();
      p_mode  = kxpa.getMode();
      p_fault = kxpa.getFaultCodes();
      p_volt  = kxpa.getVoltage();
    }

    // 3. Handle CAT Control (only if not doing manual override)
    bool catOk = cat.isConnected();
    if (catOk && !manualReq) {
      String freqStr = cat.sendCommand(CAT_COMMAND);
      if (freqStr.length() > 0) {
        long freq = freqStr.toInt();
        int newIdx = kxpa.getBandIndexByFrequency(freq);
        
        if (newIdx >= MIN_POS && newIdx <= MAX_POS && newIdx != currentBandIdx) {
          kxpa.setBand(newIdx);
          currentBandIdx = newIdx;
          p_bandName = kxpa.getBandName(currentBandIdx);
        }
      }
    }

    // 4. Update Shared State with Dirty Flags
    if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
      bool connChanged = (sharedState.kxpaConnected != kxpaOk) || 
                        (sharedState.catConnected != catOk);
      
      sharedState.kxpaConnected = kxpaOk;
      sharedState.catConnected = catOk;
      
      // Only update if not racing with a new manual request
      if (!sharedState.manualChangeReq) {
        if (sharedState.bandIndex != currentBandIdx || sharedState.bandName != p_bandName) {
          sharedState.bandIndex = currentBandIdx;
          sharedState.bandName = p_bandName;
          sharedState.bandDirty = true;
        }
      }

      // Set dirty flags for changed values
      if (sharedState.power != p_power) {
        sharedState.power = p_power;
        sharedState.powerDirty = true;
      }
      if (sharedState.temp != p_temp) {
        sharedState.temp = p_temp;
        sharedState.tempDirty = true;
      }
      if (sharedState.swr != p_swr) {
        sharedState.swr = p_swr;
        sharedState.swrDirty = true;
      }
      if (sharedState.antenna != p_ant) {
        sharedState.antenna = p_ant;
        sharedState.antennaDirty = true;
      }
      if (sharedState.mode != p_mode) {
        sharedState.mode = p_mode;
        sharedState.modeDirty = true;
      }
      if (sharedState.faults != p_fault) {
        sharedState.faults = p_fault;
        sharedState.faultsDirty = true;
      }
      if (sharedState.voltage != p_volt) {
        sharedState.voltage = p_volt;
        sharedState.voltageDirty = true;
      }
      
      if (connChanged) {
        sharedState.connectionDirty = true;
      }
      
      xSemaphoreGive(dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// -----------------------------------------------------------------------------------------
// LOOP (Core 1)
// Handles UI and Buttons
// -----------------------------------------------------------------------------------------
void loop() {
  M5.update();
  
  // --- Local Data Copies ---
  bool s_kxpaConn = false;
  bool s_catConn = false;
  int s_bandIdx = 0;
  String s_bandName, s_pwr, s_temp, s_swr, s_ant, s_mode, s_fault, s_volt;
  bool anyDirty = false;

  // Read Shared Data safely
  if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
    s_kxpaConn = sharedState.kxpaConnected;
    s_catConn = sharedState.catConnected;
    s_bandIdx = sharedState.bandIndex;
    s_bandName = sharedState.bandName;
    s_pwr = sharedState.power;
    s_temp = sharedState.temp;
    s_swr = sharedState.swr;
    s_ant = sharedState.antenna;
    s_mode = sharedState.mode;
    s_fault = sharedState.faults;
    s_volt = sharedState.voltage;
    
    // Check if anything is dirty
    anyDirty = sharedState.bandDirty || sharedState.powerDirty || 
               sharedState.tempDirty || sharedState.swrDirty ||
               sharedState.antennaDirty || sharedState.modeDirty ||
               sharedState.faultsDirty || sharedState.voltageDirty ||
               sharedState.connectionDirty;
    
    xSemaphoreGive(dataMutex);
  }

  // --- POWER OFF LOGIC with Warning ---
  if (s_kxpaConn) {
    timerLastKxpaConnection = millis();
    powerOffWarningShown = false;
  }

  unsigned long disconnectTime = millis() - timerLastKxpaConnection;
  
  // Show warning at 25 seconds
  if (disconnectTime > POWEROFF_WARNING_MS && !powerOffWarningShown) {
    showPowerOffWarning();
    powerOffWarningShown = true;
  }
  
  // Power off at 30 seconds (unless button pressed to abort)
  if (disconnectTime > POWEROFF_TIMEOUT_MS) {
    M5.update();
    if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) {
      Serial.println("Power-off aborted by user");
      timerLastKxpaConnection = millis(); // Reset timer
      powerOffWarningShown = false;
    } else {
      Serial.println("KXPA timeout: Powering off.");
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setTextColor(RED);
      M5.Lcd.drawString("Powering Off...", 80, 100);
      delay(1000);
      M5.Power.powerOff();
    }
  }

  // --- Sync & UI Logic ---
  if (!uiUpdatingBand) {
    uiBandCounter = s_bandIdx;
  }

  // --- Button Handling (Non-Blocking) ---
  bool manualAction = false;

  if (!s_catConn) {
    // Check Btn A (Up)
    if (M5.BtnA.wasPressed()) {
       if (uiBandCounter < MAX_POS) uiBandCounter++;
       btnRepeatTimer = millis() + BTN_REPEAT_DELAY_INITIAL_MS;
       manualAction = true;
    } else if (M5.BtnA.isPressed() && millis() > btnRepeatTimer) {
       if (uiBandCounter < MAX_POS) uiBandCounter++;
       btnRepeatTimer = millis() + BTN_REPEAT_RATE_MS;
       manualAction = true;
    }

    // Check Btn C (Down)
    if (M5.BtnC.wasPressed()) {
       if (uiBandCounter > MIN_POS) uiBandCounter--;
       btnRepeatTimer = millis() + BTN_REPEAT_DELAY_INITIAL_MS;
       manualAction = true;
    } else if (M5.BtnC.isPressed() && millis() > btnRepeatTimer) {
       if (uiBandCounter > MIN_POS) uiBandCounter--;
       btnRepeatTimer = millis() + BTN_REPEAT_RATE_MS;
       manualAction = true;
    }
    
    // Check Btn B (OK/Set)
    if (M5.BtnB.wasPressed()) {
      if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
        sharedState.manualTargetBand = uiBandCounter;
        sharedState.manualChangeReq = true;
        
        // Optimistic update
        sharedState.bandIndex = uiBandCounter;
        sharedState.bandName = kxpa.getBandName(uiBandCounter);
        sharedState.bandDirty = true;
        
        xSemaphoreGive(dataMutex);
      }
      
      s_bandIdx = uiBandCounter;
      s_bandName = kxpa.getBandName(uiBandCounter);
      
      uiUpdatingBand = false;
      timerDisplay = 0;
    }
  }

  if (manualAction) {
    uiUpdatingBand = true;
    timerDisplay = 0; 
  }

  // --- Display Update Logic (with Dirty Flags) ---
  bool forceUpdate = (millis() - timerDisplay > DISPLAY_UPDATE_MS) || (timerDisplay == 0);
  
  if (forceUpdate || anyDirty) {
    timerDisplay = millis();

    if (!s_kxpaConn) {
      showStatusLine("No KXPA100", RED);
      img1.fillSprite(WHITE); img1.pushSprite(0, 30);
      img1a.fillSprite(WHITE); img1a.pushSprite(160, 30);
      img2.fillSprite(WHITE); img2.pushSprite(0, 211);
      
      // Clear dirty flags
      if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
        sharedState.connectionDirty = false;
        xSemaphoreGive(dataMutex);
      }
      return;
    }

    // Top Status Line & Bottom Menu (only if connection state changed)
    if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
      if (sharedState.connectionDirty || forceUpdate) {
        if (s_catConn) {
          showStatusLine(">>  CAT Control  <<", DARKGREEN);
          img2.fillSprite(DARKGREEN);
        } else {
          showStatusLine(">>  Manual Control  <<", BLUE);
          img2.fillSprite(BLUE);
          img2.setTextColor(WHITE);
          img2.drawString("Band -", 30, 4);
          img2.drawString("OK", 138, 4);
          img2.drawString("Band +", 228, 4);
        }
        img2.pushSprite(0, 211);
        sharedState.connectionDirty = false;
      }
      xSemaphoreGive(dataMutex);
    }

    String dispBandName = uiUpdatingBand ? kxpa.getBandName(uiBandCounter) : s_bandName;
    
    drawLeftSprite(dispBandName, s_pwr, s_temp, s_swr, s_catConn);
    drawRightSprite(s_ant, s_mode, s_fault, s_volt);
    
    // Clear all dirty flags
    if (xSemaphoreTake(dataMutex, SEMAPHORE_TIMEOUT_TICKS)) {
      sharedState.bandDirty = false;
      sharedState.powerDirty = false;
      sharedState.tempDirty = false;
      sharedState.swrDirty = false;
      sharedState.antennaDirty = false;
      sharedState.modeDirty = false;
      sharedState.faultsDirty = false;
      sharedState.voltageDirty = false;
      xSemaphoreGive(dataMutex);
    }
  }
}

// -----------------------------------------------------------------------------------------
// HELPER FUNCTIONS
// -----------------------------------------------------------------------------------------

void showPowerOffWarning() {
  M5.Lcd.fillRect(0, 80, 320, 80, RED);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.drawString("KXPA DISCONNECTED", 20, 90);
  M5.Lcd.drawString("Press any button", 30, 120);
  M5.Lcd.drawString("to abort shutdown", 25, 140);
}

void showStatusLine(const String& text, int color) {
  img0.fillSprite(color);
  img0.setFont(&fonts::FreeSans12pt7b);
  img0.setTextColor(WHITE);
  
  int16_t textWidth  = img0.textWidth(text);
  int16_t textHeight = img0.fontHeight();
  int16_t x = (img0.width()  - textWidth)  / 2;
  int16_t y = (img0.height() - textHeight) / 2;

  img0.drawString(text, x, y);
  img0.pushSprite(0, 0);
}

void drawLeftSprite(const String& band, const String& power, const String& temp, const String& swr, bool catConn) {
  img1.fillSprite(WHITE);
  img1.setFont(&fonts::FreeSansBold24pt7b);
  
  if (!catConn)
    img1.setTextColor(uiUpdatingBand ? RED : DARKGREY);
  else
    img1.setTextColor(DARKGREY);
    
  img1.drawString(band, LINE_LEFT_X, LINE1_Y);
  
  img1.setFont(&fonts::FreeSansBold12pt7b);
  img1.setTextColor(DARKGREY);
  img1.drawString("Power", LINE_LEFT_X, LINE2_Y);
  img1.drawString(power, VALUES_X, LINE2_Y);
  img1.drawString("Temp.", LINE_LEFT_X, LINE3_Y);
  img1.drawString(temp, VALUES_X, LINE3_Y);
  img1.drawString("SWR", LINE_LEFT_X, LINE4_Y);
  img1.drawString(swr, VALUES_X, LINE4_Y);

  img1.pushSprite(0, 30);
}

void drawRightSprite(const String& antRaw, const String& mode, const String& faults, const String& voltage) {
  img1a.fillSprite(WHITE);
  
  String ant = antRaw;
  ant.replace("^AN1;", "ANT1");
  ant.replace("^AN2;", "ANT2");
  ant.replace("^AN1", "ANT1");
  ant.replace("^AN2", "ANT2");

  img1a.setFont(&fonts::FreeSansBold24pt7b);
  img1a.setTextColor(DARKGREY);
  img1a.drawString(ant, LINE_LEFT_X, LINE1_Y);
  
  img1a.setFont(&fonts::FreeSansBold12pt7b);
  img1a.setTextColor(DARKGREY);
  img1a.drawString(mode, LINE_LEFT_X, LINE2_Y);
  img1a.drawString(faults, LINE_LEFT_X, LINE3_Y);
  img1a.drawString("Supply " + voltage + "V", LINE_LEFT_X, LINE4_Y);
  
  img1a.pushSprite(160, 30);
}