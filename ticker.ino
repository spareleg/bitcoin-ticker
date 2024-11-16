#include "SPI.h" 

  
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// !!! Library versions should match to mentioned below !!!
// !!!!!!!!!!!! Otherwise code won't work !!!!!!!!!!!!!!!!!
//---------------------------------------+------------------+-------------------+------- +
#include <ESP8266WiFiMulti.h> // Board   | esp8266          | ESP8266 Community | 2.4.2  | TODO: migrate to the latest
#include <Timezone.h>         // Library | Timezone         | Jack Christensen  | 1.2.*  | currently the latest
#include <time.h>             // Library | Time             | Michael Margolis  | 1.6.*  | currently the latest
#include <WebSocketsClient.h> // Library | WebSockets       | Markus Sattler    | 2.3.6  | TODO: migrate to the latest
#include <ArduinoJson.h>      // Library | ArduinoJson      | Benoit Blanchon   | 6.9.1  | TODO: migrate to the latest
#include "Adafruit_GFX.h"     // Library | Adafruit GFX     | Adafruit          | 1.11.* | currently the latest
#include "Adafruit_ILI9341.h" // Library | Adafruit ILI9341 | Adafruit          | 1.6.*  | currently the latest
//---------------------------------------+------------------+-------------------+--------+
// To install library in Arduino IDE: Sketch -> Include Library -> Manage Libraries
// To install board in Arduino IDE: Tools -> Board -> Boards Manager    
// If it's not in the list add "http://arduino.esp8266.com/stable/package_esp8266com_index.json" 
// to the additional board manager URLS in the settings

// ---------- Upload settings ---------
// Board: NodeMCU 1.0 (ESP-12E Module)
// Upload Speed: 460800
// Cpu Freq: 160MHz
// ------------------------------------

// Wi-Fi connection settings:
const char* ssid     = ""; // wi-fi host
const char* password = ""; // wi-fi password

// Time Zone:
TimeChangeRule summer = {"EEST", Last, Sun, Mar, 3, 180}; 
TimeChangeRule standard = {"EET ", Last, Sun, Oct, 4, 120};
const char* weekDay[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* monthName[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// REST API DOCS: https://github.com/binance-exchange/binance-official-api-docs/blob/master/rest-api.md
const char* restApiHost = "api.binance.com";
const byte candlesLimit = 24;
const byte timeframes = 4;
const char* candlesTimeframes[timeframes] = {"3m", "1h", "1d", "1w"};
const uint16_t volColor = 0x22222a;

// WS API DOCS: https://github.com/binance-exchange/binance-official-api-docs/blob/master/web-socket-streams.md
const char* wsApiHost = "stream.binance.com";
const int wsApiPort = 9443;

// Layout:
const byte topPanel = 22;
const byte bottomPanel = 36;

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;
StaticJsonDocument<8750> jsonDoc;
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
unsigned long lastCandleOpenTime = 0;
float ph; // Price High
float pl; // Price Low
float vh; // Volume High
float vl; // Volume Low

void setup() {
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

unsigned long lastPrintTime = 0;
byte currentTimeframe = 0;
void loop() {
  unsigned long currentMs = millis();

  if (currentMs - lastPrintTime >= 35000 && second(time(nullptr)) < 25) {
    lastPrintTime = currentMs;
    printTime();
    currentTimeframe++;
    if (currentTimeframe == timeframes) currentTimeframe = 0;
    webSocket.disconnect();
    webSocket.beginSSL(wsApiHost, wsApiPort, getWsApiUrl().c_str());
    while (!requestRestApi()) {}
    drawCandles();
  }

	webSocket.loop();
}

void printTime() {
  time_t now = myTZ.toLocal(time(nullptr), &tcr);
  tft.fillRect(0, 0, 320, topPanel, ILI9341_BLACK);
  tft.setCursor(-1, 0);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
 
  tft.printf("%02d-%02d-%02d %s %02d:%02d", 
    year(now)-2000, month(now), day(now), 
    weekDay[weekday(now)], 
    hour(now), minute(now)
  );
}

String getRestApiUrl() {
  return "/api/v1/klines?symbol=BTCUSDT&interval=" + String(candlesTimeframes[currentTimeframe]) +
         "&limit=" + String(candlesLimit);
}

String getWsApiUrl() {
  return "/ws/btcusdt@kline_" + String(candlesTimeframes[currentTimeframe]);
}

unsigned int wsFails = 0;

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
      DeserializationError err = deserializeJson(jsonDoc, payload);
      if (err) {
        error(err.c_str());
        break;
      }
      unsigned int openTime = jsonDoc["k"]["t"];
      if (openTime == 0) {
        error("Got empty object from WS API");
        break;
      }
      bool candleIsNew = openTime > lastCandleOpenTime;
      if (candleIsNew) {
        lastCandleOpenTime = openTime;
        for (int i = 1; i < candlesLimit; i++) {
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
          candles[candlesLimit-1].l < pl || candles[candlesLimit-1].h > ph ||
          candles[candlesLimit-1].v < vl || candles[candlesLimit-1].v > vh)
      {
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
      for (int i = 0; i < candlesLimit; i++) {
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
  vl = candles[0].v;
  for (int i = 0; i < candlesLimit; i++) {
    if (candles[i].h > ph) ph = candles[i].h;
    if (candles[i].l < pl) pl = candles[i].l;
    if (candles[i].v > vh) vh = candles[i].v;
    if (candles[i].v < vl) vl = candles[i].v;
  }

  // Draw bottom panel with price, high and low:
  drawPrice();

  // Draw candles:
  for (int i = 0; i < candlesLimit; i++) {
    drawCandle(i);
  }
}

// Remap dollars data to pixels
int getY(float val, float minVal, float maxVal) {
  return round(map(val, minVal, maxVal, 235 - bottomPanel, topPanel + 2));
}

// Data format: [[TS, OPEN, HIGH, LOW, CLOSE, VOL, ...]]
void drawCandle(int i) {
  int oy = getY(candles[i].o, pl, ph);
  int hy = getY(candles[i].h, pl, ph);
  int ly = getY(candles[i].l, pl, ph);
  int cy = getY(candles[i].c, pl, ph);
  int vy = getY(candles[i].v, vl, vh);
  int prevVY = vy;
  if (i != 0) {
    prevVY = getY(candles[i-1].v, vl, vh);
  }

  float w = 320.0 / candlesLimit;
  float center = w / 2.0;
  center += (i * w);
  uint16_t color = cy > oy ? ILI9341_RED : ILI9341_GREEN;

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
  int bodyHeight = abs(cy - oy);
  if (bodyHeight < 1) bodyHeight = 1; // at least 1px, if candle body not formed yet
  tft.fillRect(center - 3, min(oy, cy), 7, bodyHeight, color);
}

// To track if chanded:
int lastPrice = -1;
float lastLow = -1;
float lastHigh = -1;
int lastTimeframe = -1;

void drawPrice() {
  int price = round(candles[candlesLimit-1].c);
  if (lastPrice != price) {
    tft.fillRect(0, 240 - bottomPanel, 197, bottomPanel, ILI9341_BLACK);
    tft.setCursor(0, 240 - bottomPanel);
    tft.setTextSize(5);
    tft.setTextColor(price > lastPrice ? ILI9341_GREEN : ILI9341_RED);
    lastPrice = price;
    tft.print(formatPrice(price));
  }
  if (ph != lastHigh) {
    tft.fillRect(197, 240 - bottomPanel, 113, bottomPanel / 2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    lastHigh = ph;
    tft.setCursor(197, 240 - bottomPanel);
    tft.setTextSize(2);
    tft.printf("H%*s", 7, formatPrice(round(ph)).c_str());
  }
  if (pl != lastLow) {
    tft.fillRect(197, 240 - bottomPanel / 2, 113, bottomPanel / 2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    lastLow = pl;
    tft.setCursor(197, 243 - floor(bottomPanel / 2));
    tft.setTextSize(2);
    tft.printf("L%*s", 7, formatPrice(round(pl)).c_str());
  }
  if (lastTimeframe != currentTimeframe) {
    lastTimeframe = currentTimeframe;
    tft.fillRect(310, 240 - bottomPanel, 10, bottomPanel, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(2);
    String timeframe = candlesTimeframes[currentTimeframe];
    tft.setCursor(310, 240 - bottomPanel);
    tft.print(timeframe[0]);
    tft.setCursor(310, 243 - floor(bottomPanel / 2));
    tft.print(timeframe[1]);
  }
}

String formatPrice(int n) {
  char snum[6];
  sprintf(snum, "%6d", n);
  replaceZeros(snum);
  return snum;
}

// Replaces zeros with capital letter "o"
void replaceZeros(char num[]) {
  for (int i = 0; i< strlen(num); i++) {
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
  lastPrice = lastLow = lastHigh = lastTimeframe = -1;
  drawCandles();
}
