#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- LoRa / SPI pins ---
#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26

// --- I/O ---
#define BUTTON_PIN 25
#define LED_PIN 2
#define BUZZER_PIN 4

// --- I2C LCD setup ---
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Change 0x27 → 0x3F if your LCD has a different address

// --- Press / timing thresholds (ms) ---
#define LONG_PRESS_MS 400
#define LETTER_GAP_MS 1000   // 3s letter gap
#define WORD_GAP_MS   3000   // 5s word gap
#define MSG_GAP_MS    000   // 7s send full message

// --- Globals ---
unsigned long pressStart = 0;
unsigned long lastAction = 0;
bool buttonPressed = false;
String morseBuffer = "";
int gapState = 0; // 0=none, 1=letter, 2=word

// --- Wi-Fi credentials ---
const char* WIFI_SSID     = "ShasheyAwey";
const char* WIFI_PASSWORD = "pollywag123";

// --- Your Node.js server URL ---
const char* SERVER_URL = "http://10.107.79.125:4000/api/transmit-morse";  
const char* SERVER_RECEIVE_URL = "http://10.207.202.98:4000/api/receive-morse";


// --- LED queue ---
struct MorseSymbol { char type; unsigned long start; bool active; };
#define MAX_SYMBOLS 40
MorseSymbol ledQueue[MAX_SYMBOLS];
int ledQueueStart = 0;
int ledQueueEnd = 0;

// --- Morse Table ---
struct MorseEntry { char letter; const char *code; };
MorseEntry morseTable[] = {
  {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
  {'E', "."}, {'F', "..-."}, {'G', "--."}, {'H', "...."},
  {'I', ".."}, {'J', ".---"}, {'K', "-.-"}, {'L', ".-.."},
  {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."},
  {'Q', "--.-"},{'R', ".-."},{'S', "..."}, {'T', "-"},
  {'U', "..-"},{'V', "...-"},{'W', ".--"},{'X', "-..-"},
  {'Y', "-.--"},{'Z', "--.."},
  {'1', ".----"},{'2', "..---"},{'3', "...--"},{'4', "....-"},
  {'5', "....."},{'6', "-...."},{'7', "--..."},{'8', "---.."},
  {'9', "----."},{'0', "-----"}
};
#define MORSE_TABLE_SIZE (sizeof(morseTable) / sizeof(MorseEntry))

// --- Function Prototypes ---
String morseToText(String morse);
void enqueueSymbol(char type, unsigned long now);
void updateLed(unsigned long now);
void sendImmediatePacket(const String &s);

void sendToWebSocketServer(const String &code, const String &translation) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi not connected, skipping HTTP send.");
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"code\"😕"" + code + "\",\"translation\"😕"" + translation + "\"}";
  int httpResponseCode = http.POST(json);

  if (httpResponseCode > 0) {
    Serial.printf("🌍 Sent to WebSocket Server — HTTP %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println(response);
  } else {
    Serial.printf("❌ HTTP request failed — code: %d\n", httpResponseCode);
  }

  http.end();
}

void sendToReceiveServer(const String &code, const String &translation, int signal = 0) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi not connected, skipping HTTP send to receiver.");
    return;
  }

  HTTPClient http;
  http.begin(SERVER_RECEIVE_URL);
  http.addHeader("Content-Type", "application/json");

  // JSON with an extra signal field
  String json = "{\"code\"😕"" + code + "\",\"translation\"😕"" + translation + "\",\"signal\":" + String(signal) + "}";
  int httpResponseCode = http.POST(json);

  if (httpResponseCode > 0) {
    Serial.printf("🌍 Sent to Receiver Server — HTTP %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println(response);
  } else {
    Serial.printf("❌ Receiver HTTP request failed — code: %d\n", httpResponseCode);
  }

  http.end();
}



void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== LoRa Morse Two-Way (LED Sync + LCD Integration) ===");

  // I/O
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Mission ImMORSEable!");
  delay(1500);
  lcd.clear();

  // LoRa init
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("❌ LoRa init failed!");
    lcd.setCursor(0, 0);
    lcd.print("LoRa Init Failed!");
    while (true);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setTxPower(17);

  Serial.println("✅ LoRa ready.");
  lcd.setCursor(0, 0);
  lcd.print("LoRa Connected!");
  delay(1000);
  lcd.clear();

  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Wi-Fi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  unsigned long now = millis();

  // --- RECEIVE packets ---
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) incoming += (char)LoRa.read();

    for (char c : incoming) {
      if (c == '.' || c == '-') {
        enqueueSymbol(c, now);
        Serial.print("📩 RX Symbol: "); Serial.println(c);
      } else if (c == ' ') {
        Serial.println("(RX letter gap)");
      } else if (c == '|') {
        Serial.println("(RX word gap)");
      }
    }

    if (incoming.length() > 0) {
      String decoded = morseToText(incoming);
      Serial.println("------------------------------------------");
      Serial.print("📩 RX Morse: "); Serial.println(incoming);
      Serial.print("🗣️  Translation: "); Serial.println(decoded);
      Serial.print("📶 RSSI: "); Serial.println(LoRa.packetRssi());
      Serial.println("------------------------------------------");

      // ✅ Show received message on LCD
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("RX:");
      lcd.setCursor(4, 0);
      lcd.print(incoming.substring(0, 12)); // Morse limited to fit LCD
      lcd.setCursor(0, 1);
      lcd.print(decoded.substring(0, 16));
    }
  }

  // --- BUTTON handling ---
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed && !buttonPressed) {
    pressStart = now;
    buttonPressed = true;
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
  }

  if (!pressed && buttonPressed) {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    unsigned long pressDur = now - pressStart;
    char sym = (pressDur < LONG_PRESS_MS) ? '.' : '-';
    morseBuffer += sym;

    enqueueSymbol(sym, now);
    sendImmediatePacket(String(sym));

    Serial.print("Entered: "); Serial.println(sym);
    Serial.print("Current Morse: "); Serial.println(morseBuffer);

    lastAction = now;
    buttonPressed = false;
    gapState = 0;
  }

  // --- GAP logic ---
  unsigned long idle = now - lastAction;
  if (!pressed && !buttonPressed && morseBuffer.length() > 0) {
    if (gapState == 0 && idle >= LETTER_GAP_MS && idle < WORD_GAP_MS) {
      morseBuffer += ' ';
      Serial.println("(Letter gap inserted)");
      sendImmediatePacket(" ");
      gapState = 1;
    }
    else if ((gapState == 0 || gapState == 1) && idle >= WORD_GAP_MS && idle < MSG_GAP_MS) {
      if (gapState == 1 && morseBuffer.endsWith(" ")) morseBuffer.remove(morseBuffer.length() - 1);
      morseBuffer += '|';
      Serial.println("(Word gap inserted)");
      sendImmediatePacket("|");
      gapState = 2;
    }
  }

  // --- SEND full message ---
  if (morseBuffer.length() > 0 && (now - lastAction) >= MSG_GAP_MS) {
    String humanPrint = morseBuffer;
    humanPrint.replace("|", "  ");
    String decoded = morseToText(morseBuffer);

    Serial.println("==========================================");
    Serial.print("📤 TX Morse: "); Serial.println(humanPrint);
    Serial.print("🗣️  TX Text: "); Serial.println(decoded);
    Serial.println("==========================================");

    sendImmediatePacket(morseBuffer);

    sendToWebSocketServer(morseBuffer, decoded);
    sendToReceiveServer(morseBuffer, decoded, LoRa.packetRssi());


    // ✅ Display sent message on LCD
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("TX:");
    lcd.setCursor(4, 0);
    lcd.print(humanPrint.substring(0, 12));
    lcd.setCursor(0, 1);
    lcd.print(decoded.substring(0, 16));

    morseBuffer = "";
    gapState = 0;
  }

  updateLed(now);
}

// --- Send LoRa packet ---
void sendImmediatePacket(const String &s) {
  LoRa.beginPacket();
  LoRa.print(s);
  LoRa.endPacket();
  delay(30);
}

// --- LED blink queue ---
void enqueueSymbol(char type, unsigned long now) {
  ledQueue[ledQueueEnd] = { type, now, true };
  ledQueueEnd = (ledQueueEnd + 1) % MAX_SYMBOLS;
}

void updateLed(unsigned long now) {
  if (ledQueueStart == ledQueueEnd) return;
  MorseSymbol &sym = ledQueue[ledQueueStart];
  unsigned long dur = (sym.type == '.') ? 150 : 450;

  if (sym.active && now - sym.start < dur) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
    if (sym.active) {
      sym.active = false;
      sym.start = now;
    } else if (now - sym.start >= 150) {
      ledQueueStart = (ledQueueStart + 1) % MAX_SYMBOLS;
    }
  }
}

// --- Morse to Text Decoder ---
String morseToText(String morse) {
  String result = "";
  String curr = "";
  for (unsigned int i = 0; i < morse.length(); ++i) {
    char c = morse[i];
    if (c == '.' || c == '-') curr += c;
    else if (c == ' ') {
      if (curr.length()) {
        for (int j = 0; j < MORSE_TABLE_SIZE; ++j)
          if (curr == morseTable[j].code) { result += morseTable[j].letter; break; }
        curr = "";
      }
    } else if (c == '|') {
      if (curr.length()) {
        for (int j = 0; j < MORSE_TABLE_SIZE; ++j)
          if (curr == morseTable[j].code) { result += morseTable[j].letter; break; }
        curr = "";
      }
      result += ' ';
    }
  }
  if (curr.length()) {
    for (int j = 0; j < MORSE_TABLE_SIZE; ++j)
      if (curr == morseTable[j].code) { result += morseTable[j].letter; break; }
  }
  return result;
}