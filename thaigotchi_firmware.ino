/*
 * Thaigotchi v3 - มฤคนที
 * Updates from v2:
 *   - Short press: power on/off (deep sleep toggle)
 *   - Boot sequence: idle → talking (during welcome.wav) → idle
 *   - Hold press: talking animation while listening
 *   - playWav now blocks updateAnimation properly
 *   - GPS location sent to app over BLE every 2s
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include "esp_mac.h"
#include "animations.h"

// ============ PINS ============
#define PIN_OLED_SDA      21
#define PIN_OLED_SCL      22
#define PIN_GPS_RX        16
#define PIN_GPS_TX        17
#define PIN_AMP_LRC       14
#define PIN_AMP_BCLK      32   // GPIO 32 (changed from 12 — strapping issue)
#define PIN_AMP_DIN       13
#define PIN_SD_CS          5
#define PIN_SD_SCK        18
#define PIN_SD_MISO       19
#define PIN_SD_MOSI       23
#define PIN_BUTTON         4
#define BUTTON_WAKEUP_GPIO GPIO_NUM_4

// ============ BLE ============
#define SERVICE_UUID         "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_DEVICE_STATE    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_GPS             "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define CHAR_BATTERY         "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CHAR_COMMAND         "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define CHAR_BUTTON_EVENT    "beb5483e-36e1-4688-b7f5-ea07361b26ac"
#define CHAR_AUDIO_STREAM    "beb5483e-36e1-4688-b7f5-ea07361b26ad"
#define DEVICE_NAME "Thaigotchi-Maru"

// ============ TIMING ============
#define BUTTON_DEBOUNCE_MS    50
#define BUTTON_HOLD_MIN_MS    500

// ============ I2S ============
#define I2S_AMP_PORT      I2S_NUM_0
#define SAMPLE_RATE       16000

// ============ SCREEN POWER SAVE ============
#define SCREEN_TIMEOUT_MS  60000   // dim OLED after 1m of no activity
#define SAD_TIMEOUT_MS     180000  // 3 min no interaction → sad/sleepy
bool screenOn = true;
unsigned long lastActivity = 0;
unsigned long lastInteraction = 0;  // for sad-mode (separate from screen)
bool isSad = false;

// ============ GEOFENCE (on-device, works without app) ============
#define UNLOCK_RADIUS_M   80.0   // within 80m = arrived, play intro

struct TempleSpot {
  const char* id;
  double lat;
  double lng;
  const char* introFile;  // WAV on SD card
  bool   visited;         // already played this power cycle?
};

// 3 temples — coords match the app's locations.json
TempleSpot temples[] = {
  { "wat_phra_kaew", 13.751500, 100.492900, "/wat_phra_kaew_intro.wav", false },
  { "wat_arun",      13.743700, 100.488900, "/wat_arun_intro.wav",      false },
  { "wat_pho",       13.746600, 100.493300, "/wat_pho_intro.wav",       false },
};
const int TEMPLE_COUNT = 3;

// ============ STATE ============
Adafruit_SH1107 oled(128, 128, &Wire, -1, 400000);
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

BLEServer* bleServer = nullptr;
BLECharacteristic* charDeviceState = nullptr;
BLECharacteristic* charGPS = nullptr;
BLECharacteristic* charBattery = nullptr;
BLECharacteristic* charCommand = nullptr;
BLECharacteristic* charButtonEvent = nullptr;
BLECharacteristic* charAudioStream = nullptr;
bool bleConnected = false;
bool connStateChanged = false;
bool justConnected = false;   // set in BLE callback, played in loop

enum PetPose {
  POSE_IDLE,
  POSE_TALKING,
  POSE_THINKING,
  POSE_SLEEPING,
  POSE_PETTING,
  POSE_OK
};

PetPose currentPose = POSE_IDLE;
uint8_t currentFrame = 0;
unsigned long lastFrameTime = 0;

// Button state
enum ButtonState { BTN_IDLE, BTN_PRESSED, BTN_HOLDING };
ButtonState btnState = BTN_IDLE;
unsigned long btnPressStart = 0;

// ============ FORWARD DECLARATIONS ============
void setupOLED();
void setupGPS();
void setupI2S();
void setupSD();
void setupBLE();
void handleButton();
void onShortPress();
void onHoldStart();
void onHoldEnd();
void updateAnimation();
void drawFrame(const unsigned char* frame);
void goToSleep();
void drawConnectionLost();
void notifyButtonEvent(const char* event);
void playWav(String filename);
void sendGPS();
void checkGeofence();
void wakeScreen();
void sleepScreen();
void updateScreenPower();
double haversine(double lat1, double lng1, double lat2, double lng2);

// ============ BLE CALLBACKS ============
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleConnected = true;
    connStateChanged = true;
    justConnected = true;   // play connected.wav in loop (not here — BLE stack context)
    Serial.println("📡 BLE Connected");
  }
  void onDisconnect(BLEServer* pServer) override {
    bleConnected = false;
    connStateChanged = true;
    Serial.println("📡 BLE Disconnected");
    pServer->getAdvertising()->start();
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String value = pChar->getValue().c_str();
    if (value.length() == 0) return;
    
    int colonIdx = value.indexOf(':');
    if (colonIdx < 0) return;
    
    String type = value.substring(0, colonIdx);
    String val = value.substring(colonIdx + 1);
    
    if (type == "pose") {
      if (val == "idle") currentPose = POSE_IDLE;
      else if (val == "talking") currentPose = POSE_TALKING;
      else if (val == "thinking") currentPose = POSE_THINKING;
      currentFrame = 0;
    }
    else if (type == "play") {
      // play WAV from SD
      String path = "/" + val;
      if (!path.endsWith(".wav")) path += ".wav";
      currentPose = POSE_TALKING;
      currentFrame = 0;
      playWav(path);
      currentPose = POSE_IDLE;
      currentFrame = 0;
    }
  }
};

// ============ PLAY WAV (with animation update) ============
void playWav(String filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.println("❌ ไม่พบไฟล์: " + filename);
    return;
  }

  Serial.println("🎶 กำลังเล่น: " + filename);
  wakeScreen();   // show talking animation during playback
  file.seek(44);  // skip WAV header

  static const uint32_t bufferSize = 512;
  uint8_t buffer[bufferSize];
  size_t bytesRead;
  size_t bytesWritten;

  // Set talking pose during playback
  currentPose = POSE_TALKING;
  currentFrame = 0;

  unsigned long lastTalkFrame = millis();
  while (file.available()) {
    bytesRead = file.read(buffer, bufferSize);
    i2s_write(I2S_AMP_PORT, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
    
    // Advance talking animation on its own timer (smooth mouth movement)
    if (screenOn && millis() - lastTalkFrame >= TALKING_FRAME_DELAY_MS) {
      const unsigned char* frame =
          (const unsigned char*)pgm_read_ptr(&TALKING_FRAMES[currentFrame]);
      drawFrame(frame);
      currentFrame = (currentFrame + 1) % TALKING_FRAME_COUNT;
      lastTalkFrame = millis();
    }
  }
  
  file.close();
  Serial.println("✅ เล่นจบแล้ว");
  
  // Return to idle
  currentPose = POSE_IDLE;
  currentFrame = 0;
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Thaigotchi v3 ===");
  
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  
  setupOLED();
  setupSD();
  setupGPS();
  setupI2S();
  setupBLE();
  
  // Show idle for 500ms
  currentPose = POSE_IDLE;
  drawFrame((const unsigned char*)pgm_read_ptr(&IDLE_FRAMES[0]));
  delay(500);
  
  // Play welcome.wav (auto sets talking pose)
  playWav("/welcome.wav");
  
  lastActivity = millis();   // start the screen-idle timer
  lastInteraction = millis();  // start the sad timer
  Serial.println("Ready!");
}

// ============ LOOP ============
void loop() {
  handleButton();
  
  // Redraw current frame when connection state flips (show/hide banner)
  if (connStateChanged) {
    connStateChanged = false;
    // Force animation to redraw with/without the banner
    currentFrame = 0;
    wakeScreen();   // show connection status change
  }
  
  // Play "connected" greeting (set by BLE callback, played safely here)
  if (justConnected) {
    justConnected = false;
    playWav("/connected.wav");
  }
  
  updateAnimation();
  // Randomly show "thinking" pose briefly while idling (personality)
  static unsigned long lastThinkPeek = 0;
  static bool thinkPeeking = false;
  static unsigned long thinkPeekStart = 0;
  
  if (currentPose == POSE_IDLE && screenOn && !thinkPeeking &&
      millis() - lastThinkPeek > 15000) {
    thinkPeeking = true;
    thinkPeekStart = millis();
    currentPose = POSE_THINKING;
    currentFrame = 0;
  }
  // end the peek after 2.5s, back to idle
  if (thinkPeeking && millis() - thinkPeekStart > 2500) {
    thinkPeeking = false;
    lastThinkPeek = millis();
    if (currentPose == POSE_THINKING) {
      currentPose = POSE_IDLE;
      currentFrame = 0;
    }
  }
  updateScreenPower();   // dim OLED after inactivity (GPS keeps running)
  
  // Sad/sleepy mode after long inactivity
  if (!isSad && currentPose == POSE_IDLE &&
      (millis() - lastInteraction > SAD_TIMEOUT_MS)) {
    isSad = true;
    currentPose = POSE_SLEEPING;
    currentFrame = 0;
    Serial.println("😴 No interaction — going sleepy");
  }
  
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }
  
  // Send GPS to app every 2 seconds
  static unsigned long lastGpsSend = 0;
  if (millis() - lastGpsSend > 2000) {
    lastGpsSend = millis();
    sendGPS();
    checkGeofence();   // on-device unlock — works even without app
  }
  
  delay(5);
}

// ============ BUTTON ============
void handleButton() {
  bool pressed = (digitalRead(PIN_BUTTON) == LOW);
  unsigned long now = millis();
  
  switch (btnState) {
    case BTN_IDLE:
      if (pressed) {
        btnPressStart = now;
        btnState = BTN_PRESSED;
        wakeScreen();   // any button touch wakes the display
        lastInteraction = millis();  // reset sad timer
        isSad = false;
      }
      break;
    
    case BTN_PRESSED:
      if (!pressed) {
        unsigned long duration = now - btnPressStart;
        // Short press (debounce < dur < hold threshold) = power toggle
        if (duration > BUTTON_DEBOUNCE_MS && duration < BUTTON_HOLD_MIN_MS) {
          onShortPress();
        }
        btnState = BTN_IDLE;
      } else if (now - btnPressStart > BUTTON_HOLD_MIN_MS) {
        btnState = BTN_HOLDING;
        onHoldStart();
      }
      break;
    
    case BTN_HOLDING:
      if (!pressed) {
        onHoldEnd();
        btnState = BTN_IDLE;
      }
      break;
  }
}

void onShortPress() {
  Serial.println("🔘 SHORT PRESS — power off (deep sleep)");
  notifyButtonEvent("short");
  goToSleep();
}

void goToSleep() {
  wakeScreen();
  // Play goodbye sound while the sleeping animation runs
  playWav("/bye.wav");
  
  // Show sleeping animation for ~10s before powering down
  currentPose = POSE_SLEEPING;
  currentFrame = 0;
  unsigned long sleepStart = millis();
  unsigned long lastSleepFrame = millis();
  while (millis() - sleepStart < 10000) {
    if (millis() - lastSleepFrame >= SLEEPING_FRAME_DELAY_MS) {
      const unsigned char* frame =
          (const unsigned char*)pgm_read_ptr(&SLEEPING_FRAMES[currentFrame]);
      drawFrame(frame);
      currentFrame = (currentFrame + 1) % SLEEPING_FRAME_COUNT;
      lastSleepFrame = millis();
    }
  }
  
  // Turn off OLED
  oled.clearDisplay();
  oled.display();
  oled.oled_command(0xAE);  // Display OFF command
  
  // Configure wake on button press (GPIO 4 = LOW when pressed)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, 0);
  
  Serial.println("💤 Deep sleep. Press button to wake.");
  Serial.flush();
  
  esp_deep_sleep_start();
  // Execution stops here until woken — then setup() runs fresh
}

void onHoldStart() {
  Serial.println("🤚 HOLD — petting");
  notifyButtonEvent("hold_start");
  currentPose = POSE_PETTING;
  currentFrame = 0;
  wakeScreen();
}

void onHoldEnd() {
  Serial.println("🤚 HOLD END — happy");
  notifyButtonEvent("hold_end");
  currentPose = POSE_OK;   // show happy "ok" after petting
  currentFrame = 0;
  // back to idle after a moment
  delay(800);
  currentPose = POSE_IDLE;
  currentFrame = 0;
}

void notifyButtonEvent(const char* event) {
  if (!bleConnected || charButtonEvent == nullptr) return;
  charButtonEvent->setValue((uint8_t*)event, strlen(event));
  charButtonEvent->notify();
}

// ============ GPS → APP ============
void sendGPS() {
  if (!bleConnected || charGPS == nullptr) return;

  bool hasFix = gps.location.isValid();
  double lat = hasFix ? gps.location.lat() : 0.0;
  double lng = hasFix ? gps.location.lng() : 0.0;
  int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;

  // JSON format expected by app: {"lat":..,"lng":..,"fix":..,"sats":..}
  char buf[96];
  snprintf(buf, sizeof(buf),
    "{\"lat\":%.6f,\"lng\":%.6f,\"fix\":%s,\"sats\":%d}",
    lat, lng, hasFix ? "true" : "false", sats);

  charGPS->setValue((uint8_t*)buf, strlen(buf));
  charGPS->notify();

  Serial.print("📍 GPS sent: ");
  Serial.println(buf);
}

// ============ ON-DEVICE GEOFENCE ============
// Distance between two GPS points in meters (Haversine formula)
double haversine(double lat1, double lng1, double lat2, double lng2) {
  const double R = 6371000.0;  // Earth radius in meters
  double dLat = radians(lat2 - lat1);
  double dLng = radians(lng2 - lng1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLng / 2) * sin(dLng / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}

// Check if we've arrived at any temple → play its intro once
void checkGeofence() {
  if (!gps.location.isValid()) return;  // no fix yet

  double myLat = gps.location.lat();
  double myLng = gps.location.lng();

  for (int i = 0; i < TEMPLE_COUNT; i++) {
    if (temples[i].visited) continue;  // already played this power cycle

    double dist = haversine(myLat, myLng, temples[i].lat, temples[i].lng);

    if (dist <= UNLOCK_RADIUS_M) {
      Serial.print("🎉 Arrived at ");
      Serial.print(temples[i].id);
      Serial.print(" (");
      Serial.print(dist, 1);
      Serial.println("m) — playing intro!");

      temples[i].visited = true;  // mark so it won't repeat
      wakeScreen();               // light up to show the arrival

      // Notify app too (if connected) so chat shows unlock
      if (bleConnected && charButtonEvent != nullptr) {
        char ev[48];
        snprintf(ev, sizeof(ev), "unlock:%s", temples[i].id);
        charButtonEvent->setValue((uint8_t*)ev, strlen(ev));
        charButtonEvent->notify();
      }

      // Play the intro story from SD — fully on-device!
      playWav(String(temples[i].introFile));
      break;  // one temple at a time
    }
  }
}

// ============ ANIMATION ============
void updateAnimation() {
  unsigned long now = millis();
  
  uint8_t frameCount;
  uint16_t frameDelay;
  const unsigned char* const* frames;
  
  switch (currentPose) {
    case POSE_IDLE:
      frameCount = IDLE_FRAME_COUNT;
      frameDelay = IDLE_FRAME_DELAY_MS;
      frames = IDLE_FRAMES;
      break;
    case POSE_TALKING:
      frameCount = TALKING_FRAME_COUNT;
      frameDelay = TALKING_FRAME_DELAY_MS;
      frames = TALKING_FRAMES;
      break;
    case POSE_THINKING:
      frameCount = THINKING_FRAME_COUNT;
      frameDelay = THINKING_FRAME_DELAY_MS;
      frames = THINKING_FRAMES;
      break;
    case POSE_SLEEPING:
      frameCount = SLEEPING_FRAME_COUNT;
      frameDelay = SLEEPING_FRAME_DELAY_MS;
      frames = SLEEPING_FRAMES;
      break;
    case POSE_PETTING:
      frameCount = PETTING_FRAME_COUNT;
      frameDelay = PETTING_FRAME_DELAY_MS;
      frames = PETTING_FRAMES;
      break;
    case POSE_OK:
      frameCount = OK_FRAME_COUNT;
      frameDelay = OK_FRAME_DELAY_MS;
      frames = OK_FRAMES;
      break;
  }
  
  if (now - lastFrameTime >= frameDelay) {
    if (screenOn) {   // only draw when screen is awake
      const unsigned char* frame = (const unsigned char*)pgm_read_ptr(&frames[currentFrame]);
      drawFrame(frame);
    }
    currentFrame = (currentFrame + 1) % frameCount;
    lastFrameTime = now;
  }
}

void drawFrame(const unsigned char* frame) {
  oled.clearDisplay();
  oled.drawBitmap(0, 0, frame, 128, 128, SH110X_WHITE);
  // Overlay "no connection" indicator if BLE disconnected
  if (!bleConnected) {
    oled.fillRect(0, 0, 128, 14, SH110X_BLACK);
    oled.setTextSize(1);
    oled.setTextColor(SH110X_WHITE);
    oled.setCursor(2, 3);
    oled.println("Connection lost");
  }
  oled.display();
}

void drawConnectionLost() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(15, 50);
  oled.println("Connection lost");
  oled.setCursor(10, 65);
  oled.println("Waiting for app...");
  oled.display();
}

// ============ SCREEN POWER (dim to save battery) ============
void wakeScreen() {
  lastActivity = millis();
  if (currentPose == POSE_SLEEPING) {
    currentPose = POSE_IDLE;   // wake up from sleepy pose
    currentFrame = 0;
  }
  if (!screenOn) {
    screenOn = true;
    oled.oled_command(0xAF);
  }
}

void sleepScreen() {
  if (screenOn) {
    screenOn = false;
    oled.clearDisplay();
    oled.display();
    oled.oled_command(0xAE);     // Display OFF (panel sleeps, ~15mA saved)
  }
}

// Called every loop — turns screen off after inactivity
// Called every loop — sleeping animation then turns screen off
void updateScreenPower() {
  if (!screenOn) return;  // already off
  
  unsigned long idle = millis() - lastActivity;
  
  // Phase 1: after 1 min idle → switch to sleeping animation
  if (idle > SCREEN_TIMEOUT_MS && currentPose != POSE_SLEEPING) {
    currentPose = POSE_SLEEPING;
    currentFrame = 0;
    Serial.println("😴 Idle — sleeping animation before screen off");
  }
  
  // Phase 2: after sleeping shows for 10s → turn screen off
  if (idle > SCREEN_TIMEOUT_MS + 10000) {
    sleepScreen();
    Serial.println("💤 Screen off (idle) — GPS still tracking");
  }
}

// ============ SETUP HELPERS ============
void setupOLED() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  bool ok = false;
  for (int i = 0; i < 5; i++) {
    if (oled.begin(0x3C, true)) { ok = true; break; }
    Serial.printf("⚠️ OLED retry %d/5...\n", i + 1);
    delay(200);
  }
  if (!ok) { Serial.println("OLED fail"); return; }
  
  oled.setRotation(2);
  oled.clearDisplay();
  oled.display();
}

void setupGPS() {
  gpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
}

void setupSD() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  // Retry up to 5 times — helps with flaky contact
  for (int i = 0; i < 5; i++) {
    if (SD.begin(PIN_SD_CS)) {
      Serial.println("✅ SD card ready");
      return;
    }
    Serial.printf("⚠️ SD retry %d/5...\n", i + 1);
    delay(200);
  }
  Serial.println("❌ SD card init failed after 5 tries");
}

void setupI2S() {
  // Speaker out
  i2s_config_t ampConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,    // Larger buffer for cleaner audio
    .dma_buf_len = 256,
    .use_apll = true,       // APLL for better timing accuracy
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t ampPins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = PIN_AMP_BCLK,
    .ws_io_num = PIN_AMP_LRC,
    .data_out_num = PIN_AMP_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_AMP_PORT, &ampConfig, 0, NULL);
  i2s_set_pin(I2S_AMP_PORT, &ampPins);
}

void setupBLE() {
  // Init BLE FIRST with base name (BT stack must start before reading MAC)
  BLEDevice::init(DEVICE_NAME);

  // Now read MAC (valid after BT stack is up)
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);

  // Show device ID on screen
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setTextColor(SH110X_WHITE);
  oled.setCursor(10, 50);
  oled.printf("Pa-Pai\n%02X%02X", mac[4], mac[5]);
  oled.display();
  delay(2000);

  // Rename with unique MAC suffix
  char uniqueName[32];
  snprintf(uniqueName, sizeof(uniqueName), "%s-%02X%02X",
           DEVICE_NAME, mac[4], mac[5]);
  esp_ble_gap_set_device_name(uniqueName);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
  
  BLEService* service = bleServer->createService(SERVICE_UUID);
  
  charDeviceState = service->createCharacteristic(CHAR_DEVICE_STATE,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charDeviceState->addDescriptor(new BLE2902());
  
  charGPS = service->createCharacteristic(CHAR_GPS,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charGPS->addDescriptor(new BLE2902());
  
  charBattery = service->createCharacteristic(CHAR_BATTERY,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charBattery->addDescriptor(new BLE2902());
  
  charCommand = service->createCharacteristic(CHAR_COMMAND,
    BLECharacteristic::PROPERTY_WRITE);
  charCommand->setCallbacks(new CommandCallbacks());
  
  charButtonEvent = service->createCharacteristic(CHAR_BUTTON_EVENT,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charButtonEvent->addDescriptor(new BLE2902());
  
  charAudioStream = service->createCharacteristic(CHAR_AUDIO_STREAM,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charAudioStream->addDescriptor(new BLE2902());
  
  service->start();
  
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  
  Serial.println("✅ BLE advertising as Thaigotchi-Maru");
}