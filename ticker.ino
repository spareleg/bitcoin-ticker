#include <TimeLib.h>
#include <time.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_ILI9341.h"

const char* ssid = "Osmium";
const char* password = "@3.1415926";

// API DOCS: https://bitfinex.readme.io/v2/reference#rest-public-candles
const char* apiHost = "api.bitfinex.com";
const int candlesLimit = 24;
const String candleTimeframes[] = {"5m", "1h", "6h", "1D"};
byte currentCandleTimeframe = 1; // index from candleTimeframes[] array
const int httpsPort = 443;
const int apiInterval = 10000; // ms
const size_t bufferSize = candlesLimit*JSON_ARRAY_SIZE(6) + JSON_ARRAY_SIZE(candlesLimit) + 1460;
const uint16_t volColor = 0x22222a;

// SPI Display setup:
#define TFT_CS D2
#define TFT_DC D1
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC);

// Layout:
const byte topPanel = 22;
const byte bottomPanel = 36;

const char* weekDay[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

void setup() {
  tft.begin();
  tft.setRotation(3);
  tft.setTextWrap(false);
  tft.fillScreen(ILI9341_BLACK);
  yield();
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.print("\nConnecting to ");
  tft.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    tft.print(".");
  }
  tft.println("\nWiFi connected");

  tft.println("\nWaiting for time");
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  while (!time(nullptr)) {
    tft.print(".");
    delay(500);
  }
  tft.fillScreen(ILI9341_BLACK);
  yield();
}

unsigned long apiPrevMs = 0;
unsigned long timePrevMs = 0;

void loop() {
  unsigned long currentMs = millis();

  if (currentMs - timePrevMs >= 35000 && second(time(nullptr)) < 25) {
    timePrevMs = currentMs;
    printTime();
    currentCandleTimeframe++;
    if (currentCandleTimeframe == 4) currentCandleTimeframe = 0;
  }
  
  if (currentMs - apiPrevMs >= apiInterval) {
    apiPrevMs = currentMs;
    requestAPI();   
  }
}

String getApiRequestUrl() {
  return "/v2/candles/trade:" + candleTimeframes[currentCandleTimeframe] + 
         ":tBTCUSD/hist?limit=" + String(candlesLimit);
}

void printTime() {
  time_t now = time(nullptr);
  tft.fillRect(0, 0, 320, topPanel, ILI9341_BLACK);
  tft.setCursor(0,0);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
  tft.print(weekDay[weekday(now)]);
  char buf[8];
  sprintf(buf, " %02d.%02d", day(now), month(now));
  tft.print(buf);
  sprintf(buf, " %02d:%02d ", hourFormat12(now), minute(now));
  tft.print(buf);
  if (isAM(now)) {
    tft.print("AM");
  } else {
    tft.print("PM");
  }
}

void requestAPI() {
  WiFiClientSecure client;
  if (!client.connect(apiHost, httpsPort)) {
    error("API request failed.\nPlease check your Wi-Fi source.");
    return;
  }
  client.print("GET " + getApiRequestUrl() + " HTTP/1.1\r\n" +
               "Host: " + apiHost + "\r\n" +
               "Accept: */*\r\n" +
               "User-Agent: Mozilla/4.0 (compatible; esp8266 Lua;)\r\n\r\n");

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line.startsWith("[")) {
      line.trim();
      drawCandles(strToJson(line));
      break;
    }
  }
}

JsonArray& strToJson(String str) {
  DynamicJsonBuffer jsonBuffer(bufferSize);
  JsonArray& data = jsonBuffer.parseArray(str);
  return data;
}


// API data format: [i][ MTS, OPEN, CLOSE, HIGH, LOW, VOLUME ]
void drawCandles(JsonArray& data) {
  if (!data.success()) {
    error("JSON parsing failed.\nAPI responded with invalid JSON data.");
    return;
  }
  int len = data.size();
  if (len == 0) {
    error("API responded with empty JSON array");
    return;
  }
  // Find highs and lows:
  float pl = data[0][4];
  float ph = data[0][3];
  float vl = data[0][5];
  float vh = data[0][5];
  for (int i=0; i < len; i++) {
    JsonArray& candle = data[i];
    float h = candle[3];
    float l = candle[4];
    float v = candle[5];
    if (h > ph) ph = h;
    if (l < pl) pl = l;
    if (v > vh) vh = v;
    if (v < vl) vl = v;
  }
  // Draw bottom panel with price, high and low:
  drawPrice(data[0][2], pl, ph);
  // Draw candles:
  int prevVY= getY(data[0][5], vl, vh);
  for (int i=0; i < len; i++) {
    // [ MTS, OPEN, CLOSE, HIGH, LOW, VOLUME ]
    JsonArray& candle = data[i];
    int oy = getY(candle[1], pl, ph);
    int cy = getY(candle[2], pl, ph);
    int hy = getY(candle[3], pl, ph);
    int ly = getY(candle[4], pl, ph);
    int vy = getY(candle[5], vl, vh);
    drawCandle(i, oy, cy, ly, hy, vy, prevVY);
    prevVY = vy;
  }
}

// Remap dollars data to pixels
int getY(float val, float minVal, float maxVal) {
  return round(map(val, minVal, maxVal, 235-bottomPanel, topPanel+2));
}

void drawCandle(int i, int oy, int cy, int ly, int hy, int vy, int prevVY) { 
  float w = 320.0/candlesLimit;
  float center = 320.0 - (w/2.0);
  center -= (i*w);
  uint16_t color = cy > oy ? ILI9341_RED : ILI9341_GREEN;

  // Background:
  tft.fillRect(center-(w/2), topPanel, ceil(w), 240-(topPanel+bottomPanel), ILI9341_BLACK);
  
  // Volume:
  tft.drawLine((center+w)-4, prevVY, center+4, vy, volColor);
  tft.drawLine(center+4, vy, center-4, vy, volColor);
  if (i == candlesLimit-1) tft.drawLine(0, vy, center-4, vy, volColor);

  // Head and tail:
  tft.drawLine(center, hy, center, ly, color);
  
  // Candle body:
  int bodyHeight = abs(cy-oy);
  if (bodyHeight < 1) bodyHeight = 1; // at least 1px, if candle body not formed yet
  tft.fillRect(center-3, min(oy,cy), 7, bodyHeight, color);
}

// To track if chanded:
float lastPrice = 0;
float lastLow = 0;
float lastHigh = 0;
byte lastCandleTimeframe = -1;

void drawPrice(float price, float low, float high) {
  if (lastPrice != price) {
    tft.fillRect(0, 240-bottomPanel, 180, bottomPanel, ILI9341_BLACK);
    tft.setCursor(0,240-bottomPanel);
    tft.setTextSize(5);
    tft.setTextColor(price > lastPrice ? ILI9341_GREEN : ILI9341_RED);   
    lastPrice = price; 
    tft.print("$");
    tft.print(round(price));
  }
  if (high != lastHigh) {
    tft.fillRect(190, 240-bottomPanel, 120, bottomPanel/2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);    
    lastHigh = high;  
    tft.setCursor(190, 240-bottomPanel);
    tft.setTextSize(2);
    tft.print("H $");
    tft.print(round(high)); 
  }  
  if (low != lastLow) {
    tft.fillRect(190, 240-bottomPanel/2, 120, bottomPanel/2, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);    
    lastLow = low;   
    tft.setCursor(190, 243-floor(bottomPanel/2));
    tft.setTextSize(2);
    tft.print("L $");
    tft.print(round(low)); 
  }
  if (lastCandleTimeframe != currentCandleTimeframe) {
    lastCandleTimeframe = currentCandleTimeframe;    
    tft.fillRect(310, 240-bottomPanel, 10, bottomPanel, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);    
    tft.setTextSize(2);
    String timeframe = candleTimeframes[currentCandleTimeframe];
    tft.setCursor(310, 240-bottomPanel);
    tft.print(timeframe[0]);
    tft.setCursor(310, 243-floor(bottomPanel/2));
    tft.print(timeframe[1]);   
  }
}

void error(String text) {
  tft.fillRect(0, topPanel, 320, 240-topPanel, ILI9341_RED);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(5, topPanel+5);
  tft.setTextWrap(true);
  tft.print(text);
  tft.setTextWrap(false);
  // Reset last data to make it redraw after error screen
  lastPrice = lastLow = lastHigh = 0; 
}

