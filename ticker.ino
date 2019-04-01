// board manager -> Esp 8266 -> ver. 2.4 (2.5 breakes wifi client requests)
// ArduinoJson lib -> ver. 6.9 (6.10 breaks websockets decoding)

#include <Timezone.h>
#include <time.h>
#include <ESP8266WiFiMulti.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9341.h"

// WiFi:
const char* ssid = "Osmium"; // wi-fi host
const char* password = "@3.1415926"; // wi-fi password

// Time Zone:
const bool time24h = true;
TimeChangeRule summer = {"EEST", Last, Sun, Mar, 3, 180}; 
TimeChangeRule standard = {"EET ", Last, Sun, Oct, 4, 120};  

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
const char* weekDay[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

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
	webSocket.beginSSL(wsApiHost, wsApiPort, getWsApiUrl());
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
    webSocket.beginSSL(wsApiHost, wsApiPort, getWsApiUrl());
    while (!requestRestApi()) {}
    drawCandles();
  }

	webSocket.loop();
}

void printTime() {
  time_t now = myTZ.toLocal(time(nullptr), &tcr);
  tft.fillRect(0, 0, 320, topPanel, ILI9341_BLACK);
  tft.setCursor(time24h ? 8 : 0, 0);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
  tft.print(weekDay[weekday(now)]);
  char buf[10];
  char* separator = time24h ? "  " : " ";
  sprintf(buf, "%s%02d.%02d", separator, day(now), month(now));
  tft.print(buf);
  sprintf(buf, "%s%02d:%02d", separator, time24h ? hour(now) : hourFormat12(now), minute(now));
  tft.print(buf);
  if (time24h) return;
  if (isAM(now)) {
    tft.print(" AM");
  } else {
    tft.print(" PM");
  }
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
    tft.fillRect(0, 240 - bottomPanel, 190, bottomPanel, ILI9341_BLACK);
    tft.setCursor(0, 240 - bottomPanel);
    tft.setTextSize(5);
    tft.setTextColor(price > lastPrice ? ILI9341_GREEN : ILI9341_RED);
    lastPrice = price;
    tft.print("$");
    tft.print(price);
  }
  if (ph != lastHigh) {
    tft.fillRect(190, 240 - bottomPanel, 120, bottomPanel / 2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    lastHigh = ph;
    tft.setCursor(190, 240 - bottomPanel);
    tft.setTextSize(2);
    tft.print("H $");
    tft.print(round(ph));
  }
  if (pl != lastLow) {
    tft.fillRect(190, 240 - bottomPanel / 2, 120, bottomPanel / 2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    lastLow = pl;
    tft.setCursor(190, 243 - floor(bottomPanel / 2));
    tft.setTextSize(2);
    tft.print("L $");
    tft.print(round(pl));
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

void error(String text) {
  tft.fillRect(0, topPanel, 320, 240 - topPanel, ILI9341_RED);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, topPanel + 15);
  tft.setTextWrap(true);
  tft.print("Holy shit!\n"+text);
  tft.setTextWrap(false);
  delay(5000);
  // Reset last data to make it redraw after error screen
  lastPrice = lastLow = lastHigh = lastTimeframe = -1;
  drawCandles();
}
