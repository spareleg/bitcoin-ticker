#include "SPI.h"

// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// !!! Library versions should match to mentioned below !!!
// !!!!!!!!!!!! Otherwise code won't work !!!!!!!!!!!!!!!!!
//---------------------------------------+------------------+-------------------+-------- +
#include <ESP8266WiFiMulti.h> // Board   | esp8266          | ESP8266 Community | 3.1.*   |
#include <Timezone.h>         // Library | Timezone         | Jack Christensen  | 1.2.*   |
#include <time.h>             // Library | Time             | Michael Margolis  | 1.6.*   |
#include <WebSocketsClient.h> // Library | WebSockets       | Markus Sattler    | 2.6.*   |
#include <ArduinoJson.h>      // Library | ArduinoJson      | Benoit Blanchon   | 7.3.*   |
#include "Adafruit_GFX.h"     // Library | Adafruit GFX     | Adafruit          | 1.10.14 | 1.11.* prints text with too much blinking
#include "Adafruit_ILI9341.h" // Library | Adafruit ILI9341 | Adafruit          | 1.6.*   |
//---------------------------------------+------------------+-------------------+---------+
// To install library in Arduino IDE: Sketch -> Include Library -> Manage Libraries
// To install board in Arduino IDE: Tools -> Board -> Boards Manager
// If it's not in the list add "http://arduino.esp8266.com/stable/package_esp8266com_index.json"
// to the additional board manager URLS in the settings

// ---------- Upload settings ---------
// Board: NodeMCU 1.0 (ESP-12E Module)
// Upload Speed: 460800
// Cpu Freq: 160MHz
// ------------------------------------

// Buttons settings
const byte currencyButtonPin = D8;
const byte timeframeButtonPin = D0;
const byte debounceDelay = 50;

// Wi-Fi connection settings:
const char* ssid     = ""; // wi-fi host
const char* password = ""; // wi-fi password

// Time Zone:
TimeChangeRule summer = {"EEST", Last, Sun, Mar, 3, 180};
TimeChangeRule standard = {"EET ", Last, Sun, Oct, 4, 120};
const char* weekDay[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// REST API DOCS: https://github.com/binance-exchange/binance-official-api-docs/blob/master/rest-api.md
const char* restApiHost = "api.binance.com";
const byte candlesLimit = 24;
const byte totalTimeframes = 5;
const char* candlesTimeframes[totalTimeframes] = {"3m", "1h", "1d", "1w", "1M"};
const byte totalCurrencies = 3;
const char* candlesCurrencies[totalCurrencies] = {"BTCUSDT", "ETHUSDT", "ETHBTC"};
// RGB565 Colors (https://rgbcolorpicker.com/565)
const uint16_t volColor = 0x000f;
const uint16_t brightRed = 0xFA08;

// WS API DOCS: https://github.com/binance-exchange/binance-official-api-docs/blob/master/web-socket-streams.md
const char* wsApiHost = "stream.binance.com";
const word wsApiPort = 9443;

// Layout:
const byte topPanel = 22;
const byte bottomPanel = 36;

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;
JsonDocument jsonDoc;
#define TFT_CS D2
#define TFT_DC D1
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);
Timezone myTZ(summer, standard);
TimeChangeRule *tcr;

typedef struct {
  float l; // Low
  float h; // High
  float o; // Open
  float c; // Close
  float v; // Volume
} Candle;
Candle candles[candlesLimit];
unsigned long long lastCandleOpenTime = 0;
float ph; // Price High
float pl; // Price Low
float vh; // Volume High

bool currencyButtonState;
bool timeframeButtonState;
bool lastCurrencyButtonState = LOW;
bool lastTimeframeButtonState = LOW;
unsigned long lastCurrencyButtonDebounceTime = 0;
unsigned long lastTimeframeButtonDebounceTime = 0;

void setup() {
  pinMode(currencyButtonPin, INPUT);
  pinMode(timeframeButtonPin, INPUT);

  // Setting display:
  tft.begin();
  tft.setRotation(3);
  tft.setTextWrap(false);
  tft.fillScreen(ILI9341_BLACK);
  yield();
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);

  // Connecting to WiFi:
  tft.print("\nConnecting to ");
  tft.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(ssid, password);
  while(WiFiMulti.run() != WL_CONNECTED) {
    tft.print(".");
    delay(500);
  }
  tft.println("\nWiFi connected");

  // Settings time:
  tft.println("\nWaiting for current time");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  while (true) {
    time_t now = time(nullptr);
    if (now && year(now) > 2017) break;
    tft.print(".");
    delay(500);
  }
  tft.fillScreen(ILI9341_BLACK);
  yield();
  printTime();

  // Load and draw all candles
  while (!requestRestApi()) {}
  drawCandles();

  // Connecting to WS:
  webSocket.beginSSL(wsApiHost, wsApiPort, getWsApiUrl().c_str());
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(1000);
}

byte currentCurrency = 0;
byte currentTimeframe = 0;

void loop() {
  printTime();

  bool currencyButtonReading = digitalRead(currencyButtonPin);
  if (currencyButtonReading != lastCurrencyButtonState) {
    lastCurrencyButtonDebounceTime = millis();
    lastCurrencyButtonState = currencyButtonReading;
  }

  bool timeframeButtonReading = digitalRead(timeframeButtonPin);
  if (timeframeButtonReading != lastTimeframeButtonState) {
    lastTimeframeButtonDebounceTime = millis();
    lastTimeframeButtonState = timeframeButtonReading;
  }

  if ((millis() - lastCurrencyButtonDebounceTime) > debounceDelay) {
    if (currencyButtonReading != currencyButtonState) {
      currencyButtonState = currencyButtonReading;
      if (currencyButtonState == HIGH) {
        currentCurrency++;
        if (currentCurrency == totalCurrencies) currentCurrency = 0;
        loadingMessage(String(candlesCurrencies[currentCurrency]).substring(0, 3));
        redrawCharts();
      }
    }
  }

  if ((millis() - lastTimeframeButtonDebounceTime) > debounceDelay) {
    if (timeframeButtonReading != timeframeButtonState) {
      timeframeButtonState = timeframeButtonReading;
      if (timeframeButtonState == HIGH) {
        currentTimeframe++;
        if (currentTimeframe == totalTimeframes) currentTimeframe = 0;
        loadingMessage(candlesTimeframes[currentTimeframe]);
        redrawCharts();
      }
    }
  }

  webSocket.loop();
}

void redrawCharts() {
  webSocket.disconnect();
  webSocket.beginSSL(wsApiHost, wsApiPort, getWsApiUrl().c_str());
  while (!requestRestApi()) {}
  drawCandles();
}

// Tracking last displayed minute to not redraw time if not changed
byte displayedMinute = 255;

void loadingMessage(String text) {
  displayedMinute = 255; // Forces to redraw time even if minute hasn't changed
  tft.fillRect(0, 0, 320, topPanel, ILI9341_BLACK);
  tft.setCursor(92, 0);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.print("Loading " + text + "...");
}

void printTime() {
  time_t now = myTZ.toLocal(time(nullptr), &tcr);
  byte currentMinute = minute(now);
  if (currentMinute == displayedMinute) return;
  displayedMinute = currentMinute;

  tft.fillRect(0, 0, 320, topPanel, ILI9341_BLACK);
  tft.setCursor(-1, 0);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);

  tft.printf("%02d-%02d-%02d %s %02d:%02d",
    year(now)-2000, month(now), day(now),
    weekDay[weekday(now)],
    hour(now), currentMinute
  );
}

String getRestApiUrl() {
  return "/api/v1/klines?symbol=" + String(candlesCurrencies[currentCurrency]) +
         "&interval=" + String(candlesTimeframes[currentTimeframe]) +
         "&limit=" + String(candlesLimit);
}

String getWsApiUrl() {
  String s = String(candlesCurrencies[currentCurrency]);
  s.toLowerCase();
  return "/ws/" + s + "@kline_" + String(candlesTimeframes[currentTimeframe]);
}

byte wsFails = 0;

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      wsFails++;
      if (wsFails > 2) error("WS disconnected");
      break;
    case WStype_CONNECTED:
      break;
    case WStype_TEXT:
      wsFails = 0;
      DeserializationError err = deserializeJson(jsonDoc, (char*) payload);
      if (err) {
        error(err.c_str());
        break;
      }
      unsigned long long openTime = jsonDoc["k"]["t"];
      if (openTime == 0) {
        error("Failed to parse JSON from WS");
        break;
      }
      bool candleIsNew = openTime > lastCandleOpenTime;
      if (candleIsNew) {
        lastCandleOpenTime = openTime;
        for (byte i = 1; i < candlesLimit; i++) {
          candles[i-1] = candles[i];
        }
      }
      candles[candlesLimit-1].o = jsonDoc["k"]["o"];
      candles[candlesLimit-1].h = jsonDoc["k"]["h"];
      candles[candlesLimit-1].l = jsonDoc["k"]["l"];
      candles[candlesLimit-1].c = jsonDoc["k"]["c"];
      candles[candlesLimit-1].v = jsonDoc["k"]["v"];

      // If we get new low/high we need to redraw all candles, otherwise just last one:
      if (candleIsNew ||
        candles[candlesLimit-1].l < pl ||
        candles[candlesLimit-1].h > ph ||
        candles[candlesLimit-1].v > vh
      ) {
        drawCandles();
      } else {
        drawPrice();
        drawCandle(candlesLimit-1);
      }
      break;
  }
}

bool requestRestApi() {
  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(restApiHost, 443)) {
    error("Internet connection lost.\nCheck the Wi-Fi source.");
    return false;
  }
  client.print("GET " + getRestApiUrl() + " HTTP/1.1\r\n" +
               "Host: " + restApiHost + "\r\n" +
               "Accept: application/json\r\n" +
               "User-Agent: Mozilla/4.0 (compatible; esp8266 Lua;)\r\n\r\n");

  while (client.connected()) {
    String line = client.readStringUntil('\r');
    line.trim();
    if (line.startsWith("[") && line.endsWith("]")) {
      DeserializationError err = deserializeJson(jsonDoc, line);
      if (err) {
        error(err.c_str());
        return false;
      } else if (jsonDoc.as<JsonArray>().size() == 0) {
        error("Empty JSON array");
        return false;
      }

      // Data format: [[TS, OPEN, HIGH, LOW, CLOSE, VOL, ...], ...]
      JsonArray _candles = jsonDoc.as<JsonArray>();
      for (byte i = 0; i < candlesLimit; i++) {
        candles[i].o = _candles[i][1];
        candles[i].h = _candles[i][2];
        candles[i].l = _candles[i][3];
        candles[i].c = _candles[i][4];
        candles[i].v = _candles[i][5];
      }
      lastCandleOpenTime = _candles[candlesLimit-1][0];
      return true;
    }
  }
  error("No JSON found in API response.");
  return false;
}

void drawCandles() {
  // Find highs and lows:
  ph = candles[0].h;
  pl = candles[0].l;
  vh = candles[0].v;
  for (byte i = 0; i < candlesLimit; i++) {
    if (candles[i].h > ph) ph = candles[i].h;
    if (candles[i].l < pl) pl = candles[i].l;
    if (candles[i].v > vh) vh = candles[i].v;
  }

  // Draw bottom panel with price, high and low:
  drawPrice();

  // Draw candles:
  for (byte i = 0; i < candlesLimit; i++) {
    drawCandle(i);
  }
}

// Remap dollars data to pixels
word getY(float val, float minVal, float maxVal) {
  float minValY = 235 - bottomPanel;
  float maxValY = topPanel + 2;
  return round((val - minVal) * (maxValY - minValY) / (maxVal - minVal) + minValY);
}

// Data format: [[TS, OPEN, HIGH, LOW, CLOSE, VOL, ...]]
void drawCandle(byte i) {
  word oy = getY(candles[i].o, pl, ph);
  word hy = getY(candles[i].h, pl, ph);
  word ly = getY(candles[i].l, pl, ph);
  word cy = getY(candles[i].c, pl, ph);
  word vy = getY(candles[i].v, 0, vh);
  word prevVY = vy;
  if (i != 0) {
    prevVY = getY(candles[i-1].v, 0, vh);
  }

  float w = 320.0 / candlesLimit;
  float center = w / 2.0;
  center += (i * w);
  uint16_t color = cy > oy ? brightRed : ILI9341_GREEN;

  // Background:
  tft.fillRect((center - w) + 5, topPanel, ceil(w), 240 - (topPanel + bottomPanel), ILI9341_BLACK);

  // Volume:
  tft.drawLine((center - w) + 5, prevVY, center - 5, vy, volColor);
  tft.drawLine(center - 4, vy, center + 4, vy, volColor);
  if (i == candlesLimit - 1) {
    tft.fillRect(center + 5, topPanel, w/2, 240 - (topPanel + bottomPanel), ILI9341_BLACK);
    tft.drawLine(center + 5, vy, 320, vy, volColor);
  }

  // Head and tail:
  tft.drawLine(center, hy, center, ly, color);

  // Candle body:
  word bodyHeight = abs(cy - oy);
  if (bodyHeight == 0) bodyHeight = 1; // at least 1px, if candle body not formed yet
  tft.fillRect(center - 3, min(oy, cy), 7, bodyHeight, color);
}

// To track if changed:
float displayedPriceF = -1;
String displayedPrice = "";
String displayedLow = "";
String displayedHigh = "";
byte displayedCurrency = 255;
byte displayedTimeframe = 255;

void drawPrice() {
  float priceF = candles[candlesLimit-1].c;
  String price = formatPrice(priceF);
  if (displayedPrice != price) {
    tft.fillRect(0, 240 - bottomPanel, 193, bottomPanel, ILI9341_BLACK);
    tft.setCursor(0, 240 - bottomPanel);
    tft.setTextSize(5);
    tft.setTextColor(priceF > displayedPriceF ? ILI9341_GREEN : brightRed);
    tft.print(price);
    displayedPrice = price;
    displayedPriceF = priceF;
  }
  String high = formatPrice(ph);
  if (high != displayedHigh) {
    tft.fillRect(195, 240 - bottomPanel, 89, bottomPanel / 2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(195, 240 - bottomPanel);
    tft.setTextSize(2);
    tft.print(high);
    displayedHigh = high;
  }
  String low = formatPrice(pl);
  if (low != displayedLow) {
    tft.fillRect(195, 240 - bottomPanel / 2, 89, bottomPanel / 2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(195, 243 - floor(bottomPanel / 2));
    tft.setTextSize(2);
    tft.print(low);
    displayedLow = low;
  }
  if (displayedTimeframe != currentTimeframe ||
      displayedCurrency  != currentCurrency
  ) {
    displayedTimeframe = currentTimeframe;
    displayedCurrency  = currentCurrency;
    tft.fillRect(286, 240 - bottomPanel, 34, bottomPanel, ILI9341_BLACK);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(286, 240 - bottomPanel);
    tft.print(String(candlesCurrencies[currentCurrency]).substring(0, 3));
    tft.setCursor(286, 243 - floor(bottomPanel / 2));
    tft.print(candlesTimeframes[currentTimeframe]);
  }
}

String formatPrice(float p) {
  char snum[6];
  if (p < 10) {
    sprintf(snum, "%6.4f", p);
  } else if (p < 1000) {
    sprintf(snum, "%6.2f", p);
  } else if (p < 10000) {
    sprintf(snum, "%6.1f", p);
  } else {
    int d = round(p);
    sprintf(snum, "%6d", d);
  }
  replaceZeros(snum);
  return snum;
}

// Replaces zeros with capital letter "o"
void replaceZeros(char num[]) {
  for (byte i = 0; i < strlen(num); i++) {
    if (num[i] == '0') num[i] = 'O';
  }
}

void error(String text) {
  tft.fillRect(0, topPanel, 320, 240 - topPanel, ILI9341_RED);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, topPanel + 15);
  tft.setTextWrap(true);
  tft.print("Holy cow!\n"+text);
  tft.setTextWrap(false);
  delay(5000);
  // Reset last data to make it redraw after error screen
  displayedPriceF = -1;
  displayedPrice = displayedLow = displayedHigh = "";
  displayedTimeframe = displayedCurrency = displayedMinute = 255;
  drawCandles();
}
