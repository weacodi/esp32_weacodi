// SPDX-License-Identifier: AGPL-3.0-only

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "WeatherMath.h"
#include "forecast_ca.h"
#include "fonts/temperatureFont.h"
#include "fonts/cityFont.h"
#include "fonts/comfortFont.h"
#include "fonts/footerFont.h"

#if __has_include("wifi.local.h")
#include "wifi.local.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

constexpr int hoursPerDay = 24;
constexpr int maxDays = 3;
constexpr int forecastHours = hoursPerDay * maxDays;
constexpr int width = 320;
constexpr int top = 43;
constexpr int bottom = 204;
constexpr int roadStrokeHeight = 3;
constexpr char cityName[] = "Munich";
constexpr char timezone[] = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr char forecastUrl[] =
    "https://weacodi.com/api/v1/forecast?latitude=48.1374&longitude=11.5755"
    "&current=temperature_2m&hourly=temperature_2m,relative_humidity_2m,cloudcover,precipitation,rain,showers,"
    "snowfall,wind_speed_10m,shortwave_radiation"
    "&daily=sunrise,sunset&past_days=2&forecast_days=3&timezone=Europe%2FBerlin";

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(33);
struct ForecastSnapshot {
  float currentTemperature;
  WeatherHour hours[forecastHours];
};
ForecastSnapshot snapshot;
int forecastCount = 0;
int forecastOffset = 0;
int selectedDays = 2;
bool hasForecast = false;
const char* forecastError = nullptr;
const char* waitingFor = nullptr;
unsigned long waitingSince = 0;
constexpr unsigned long connectionGraceMs = 15000;
constexpr unsigned long connectedScreenMs = 2000;
constexpr char wifiConnecting[] = "Connecting to WiFi...";
constexpr char wifiConnectedLabel[] = "Connecting to WiFi - connected";
constexpr char serverUnavailable[] = "Weacodi® API unavailable";
bool startupConnected = false;
unsigned long startupConnectedAt = 0;

struct ForecastRequestError {
  const char* message;
  const char* detail;
};
unsigned long nextRefresh = 0;
constexpr unsigned long rangeDisplayMs = 5000;
unsigned long rangeShownAt = 0;
bool rangeVisible = false;
time_t footerMinute = -1;
constexpr unsigned long clockHalfPeriodMs = 500;
unsigned long clockPhase = 0;
constexpr int footerY = 224;
constexpr int footerRight = width - 6;
int footerDetailLeft = 0;

int minuteOfDay(const char* stamp) {
  return ((stamp[11] - '0') * 10 + stamp[12] - '0') * 60
      + (stamp[14] - '0') * 10 + stamp[15] - '0';
}

bool parseForecast(Stream& stream, const char* currentHour) {
  JsonDocument doc;
  auto error = deserializeJson(doc, stream);
  if (error) {
    Serial.printf("JSON: %s\n", error.c_str());
    return false;
  }

  const auto hourly = doc["hourly"].as<JsonObjectConst>();
  const auto times = hourly["time"].as<JsonArrayConst>();
  const auto sunrise = doc["daily"]["sunrise"].as<JsonArrayConst>();
  const auto sunset = doc["daily"]["sunset"].as<JsonArrayConst>();
  size_t start = 0;
  while (start < times.size() && strcmp(times[start] | "", currentHour) < 0) ++start;
  if (start == times.size()) {
    Serial.printf("Missing hours from %s, index %u of %u\n", currentHour, start, times.size());
    return false;
  }

  const size_t count = min(times.size() - start, size_t(forecastHours));
  static WeatherHour parsed[forecastHours];
  for (size_t i = start; i < start + count; ++i) {
    const char* stamp = times[i] | "";
    if (strlen(stamp) != 16 || !hourly["temperature_2m"][i].is<float>() ||
        !hourly["wind_speed_10m"][i].is<float>() || !hourly["cloudcover"][i].is<float>() ||
        !hourly["precipitation"][i].is<float>() || !hourly["relative_humidity_2m"][i].is<float>()) {
      Serial.printf("Invalid hourly data at %u\n", i);
      return false;
    }
    auto& hour = parsed[i - start];
    hour = {};
    strcpy(hour.time, stamp);
    hour.temperature = hourly["temperature_2m"][i].as<float>();
    hour.wind = hourly["wind_speed_10m"][i].as<float>();
    hour.cloud = hourly["cloudcover"][i].as<float>();
    hour.humidity = hourly["relative_humidity_2m"][i].as<float>();
    hour.precipitation = hourly["precipitation"][i].as<float>();
    hour.rain = liquidPrecipitation(hourly["rain"][i] | 0.f, hourly["showers"][i] | 0.f,
                                    hour.precipitation);
    hour.snow = hourly["snowfall"][i] | 0.f;
    if (hour.rain == 0 && hour.snow <= 0) hour.rain = hour.precipitation;
    hour.radiation = hourly["shortwave_radiation"][i] | 0.f;

    for (size_t day = 0; day < sunrise.size(); ++day) {
      const char* rise = sunrise[day] | "";
      const char* set = sunset[day] | "";
      if (strlen(rise) != 16 || strlen(set) != 16 || strncmp(stamp, rise, 10) != 0) continue;
      const int first = minuteOfDay(rise), last = minuteOfDay(set), minute = minuteOfDay(stamp);
      hour.sunriseMinute = first;
      hour.sunsetMinute = last;
      hour.daylight = minute >= first && minute <= last;
      if (hour.daylight && last > first) {
        hour.sun = 1.f - fabsf(2.f * (minute - first) / (last - first) - 1.f);
      }
      break;
    }

    float water = 0;
    for (size_t j = i > hoursPerDay ? i - hoursPerDay : 0; j <= i; ++j) {
      const float total = hourly["precipitation"][j] | 0.f;
      const float liquid = liquidPrecipitation(hourly["rain"][j] | 0.f, hourly["showers"][j] | 0.f, total);
      const float amount = max(total, liquid);
      water += max(0.f, amount);
      if (j < i) {
        water = max(0.f, water - dryingRate(hourly["temperature_2m"][j] | 8.f,
                                          hourly["wind_speed_10m"][j] | 0.f));
      } else {
        hour.wet = roadIsWet(amount, water);
      }
    }
  }
  snapshot.currentTemperature = doc["current"]["temperature_2m"] | parsed[0].temperature;
  memcpy(snapshot.hours, parsed, count * sizeof(WeatherHour));
  forecastCount = count;
  forecastOffset = 0;
  return true;
}

ForecastRequestError fetchForecast(const char* currentHour) {
  WiFiClientSecure tls;
  tls.setCACert(forecastCA);
  HTTPClient http;
  http.useHTTP10(true);  // Read the JSON stream directly without chunk framing.
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  if (!http.begin(tls, forecastUrl)) {
    return {serverUnavailable, "Could not connect to the forecast server."};
  }
  const int status = http.GET();
  Serial.printf("Forecast HTTP %d\n", status);
  const bool usable = status == HTTP_CODE_OK && parseForecast(http.getStream(), currentHour);
  http.end();
  if (usable) return {nullptr, nullptr};
  if (status == HTTP_CODE_OK) return {"Forecast data missing", "Server returned no usable forecast."};
  if (status == HTTPC_ERROR_READ_TIMEOUT) return {serverUnavailable, "The forecast request timed out."};
  if (status >= 400 && status < 500) return {serverUnavailable, "Server rejected the forecast request."};
  return {serverUnavailable, "The server may be temporarily offline."};
}

int shownHours() {
  return min(selectedDays * hoursPerDay, forecastCount - forecastOffset);
}

int column(int hour) {
  return hour * width / shownHours();
}

int graphY(float percent) {
  return bottom - lroundf(constrain(percent, 0.f, 100.f) * (bottom - top) / 100.f);
}

uint16_t rainFillColor(float depth) {
  return tft.color565(0, 123 + 98 * depth, 200 + 55 * depth);
}

void drawColumn(int i) {
  const auto& hour = snapshot.hours[forecastOffset + i];
  const int x = column(i);
  const int w = column(i + 1) - x;
  const int sunY = graphY(hour.sun * 100);
  const int cloudY = graphY(hour.cloud);
  const int rainY = graphY(hour.rain * 10);
  const int rainStrokeTop = min(rainY, bottom - 1);
  const int snowTop = min(graphY(hour.snow * 10), bottom - 1);
  const int roadTop = bottom - roadStrokeHeight + 1;
  const uint16_t roadColor = hasBlackIceRisk(hour) ? TFT_WHITE : rainFillColor(1.f);

  // Keep the original ESP32 background palette beneath the weather overlays.
  for (int y = top; y <= bottom; ++y) {
    const float depth = float(y - top) / (bottom - top);
    uint16_t color;
    if (hour.daylight) {
      color = tft.color565(163 + 61 * depth, 209 + 46 * depth, 240 + 15 * depth);
      if (hour.sun > 0 && y >= sunY) {
        color = tft.color565(255, 164 + 65 * depth, 35 + 145 * depth);
      }
    } else {
      color = tft.color565(14 * depth, 20 * depth, 32 * depth);
    }
    if (hour.cloud > 0 && y >= cloudY) {
      const int grey = hour.daylight ? 170 + 80 * depth : 36 + 63 * depth;
      color = tft.color565(grey, grey, grey);
    }

    // Snow is opaque white over rain; road conditions stay above both.
    if (hour.rain > 0) {
      if (y >= rainY) color = rainFillColor(depth);
      if (y >= rainStrokeTop && y <= rainStrokeTop + 1) color = TFT_CYAN;
    }
    if (hour.snow > 0 && y >= snowTop) color = TFT_WHITE;
    if (hour.wet && y >= roadTop) {
      color = roadColor;
    }
    tft.drawFastHLine(x, y, w, color);
  }
}

void drawStep(int i, int y, int previous, uint16_t color, int riserWidth = 1) {
  const int x = column(i);
  const int w = column(i + 1) - x;
  if (i) tft.fillRect(x, min(y, previous), riserWidth, abs(y - previous) + 1, color);
  tft.fillRect(x, min(y, bottom - 1), w, 2, color);
}

// Font 2 contains ASCII only. Draw a small registered mark.
constexpr int registeredSize = 9;
constexpr int registeredWidth = registeredSize + 1;
constexpr uint16_t registeredGlyph[registeredSize] = {
  0b001111100,
  0b010000010,
  0b100111001,
  0b100101001,
  0b100110001,
  0b100101001,
  0b100101001,
  0b010000010,
  0b001111100,
};

template <typename Surface>
void drawRegisteredMark(Surface& surface, int x, int y, uint16_t color) {
  for (int row = 0; row < registeredSize; ++row) {
    for (int col = 0; col < registeredSize; ++col) {
      if (registeredGlyph[row] & (1 << (registeredSize - 1 - col))) {
        surface.drawPixel(x + col, y + row, color);
      }
    }
  }
}

void drawStatusText(const char* text, int x, int y, bool rightAligned = false) {
  const char* mark = strstr(text, "®");
  tft.setTextDatum(rightAligned ? TR_DATUM : TL_DATUM);
  if (!mark) {
    tft.drawString(text, x, y, 2);
    return;
  }
  const String prefix = String(text).substring(0, mark - text);
  const char* suffix = mark + sizeof("®") - 1;
  const int prefixWidth = tft.textWidth(prefix, 2);
  if (rightAligned) x -= prefixWidth + registeredWidth + tft.textWidth(suffix, 2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(prefix, x, y, 2);
  drawRegisteredMark(tft, x + prefixWidth, y + 2, TFT_ORANGE);
  tft.drawString(suffix, x + prefixWidth + registeredWidth, y, 2);
}

void pushSmoothedMask(TFT_eSprite& mask, int left, int topEdge, uint16_t color, bool bold) {
  const int maskWidth = mask.width();
  const int maskHeight = mask.height();
  auto ink = [&](int x, int y) {
    return x >= 0 && x < maskWidth && y >= 0 && y < maskHeight && mask.readPixelValue(x, y) == 1;
  };
  auto covered = [&](int x, int y) { return ink(x, y) || (bold && ink(x - 1, y)); };
  uint16_t row[width];
  for (int y = 0; y < maskHeight; ++y) {
    for (int x = 0; x < maskWidth; ++x) {
      const int neighbors = covered(x - 1, y) + covered(x + 1, y)
                          + covered(x, y - 1) + covered(x, y + 1);
      const uint8_t alpha = neighbors > 1 ? 100 : neighbors * 35;
      const uint16_t pixel = covered(x, y) ? color : tft.alphaBlend(alpha, color, TFT_BLACK);
      row[x] = __builtin_bswap16(pixel);
    }
    tft.pushRect(left, topEdge + y, maskWidth, 1, row);
  }
}

int drawCurrentTemperature(float temperature) {
  constexpr int left = 6;
  constexpr int unitFont = 2;
  char number[12];
  snprintf(number, sizeof(number), "%ld", lroundf(temperature));

  // The digits are rasterized from outlines at their final size, with per-pixel coverage.
  tft.loadFont(temperatureFont);
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const int degreeX = left + tft.drawString(number, left, 35);
  tft.unloadFont();
  tft.setTextDatum(TL_DATUM);

  const int unitWidth = 8 + tft.textWidth("C", unitFont);
  TFT_eSprite mask(&tft);
  mask.setColorDepth(1);
  if (mask.createSprite(unitWidth + 2, tft.fontHeight(unitFont) + 2)) {
    mask.setTextColor(TFT_WHITE, TFT_BLACK);
    mask.drawCircle(4, 10, 2, TFT_WHITE);
    mask.drawString("C", 9, 5, unitFont);
    pushSmoothedMask(mask, degreeX - 1, 0, TFT_WHITE, false);
  } else {
    tft.drawCircle(degreeX + 3, 10, 2, TFT_WHITE);
    tft.drawString("C", degreeX + 8, 5, unitFont);
  }
  return degreeX + unitWidth;
}

void drawCityName(int left) {
  tft.loadFont(cityFont);
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(cityName, left, 22);
  tft.unloadFont();
  tft.setTextDatum(TL_DATUM);
}

void drawChartBrand(int topEdge) {
  constexpr char label[] = "Weacodi";
  constexpr int font = 2;
  const int nameWidth = tft.textWidth(label, font);
  const int labelWidth = nameWidth + registeredWidth + 4;
  const int labelHeight = tft.fontHeight(font) + 2;
  const int left = width - labelWidth - 4;
  TFT_eSprite mask(&tft);
  mask.setColorDepth(1);
  if (!mask.createSprite(labelWidth, labelHeight)) return;
  mask.setTextColor(TFT_WHITE, TFT_BLACK);
  mask.drawString(label, 1, 1, font);
  pushSmoothedMask(mask, left, topEdge, TFT_WHITE, true);

  const int markLeft = left + 1 + nameWidth;
  drawRegisteredMark(tft, markLeft, topEdge + 3, TFT_WHITE);
  drawRegisteredMark(tft, markLeft + 1, topEdge + 3, TFT_WHITE);
}

void drawHeaderRight() {
  constexpr int inset = 6;
  const int y = forecastError ? 0 : 11;
  const int clearHeight = forecastError ? 30 : 29;
  tft.fillRect(width - 122, 0, 122, clearHeight, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  if (rangeVisible) {
    char range[22];
    snprintf(range, sizeof(range), "Next %d hours", shownHours());
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(range, width - inset, y, 2);
  } else {
    drawChartBrand(y);
  }
  if (forecastError) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    if (!rangeVisible) tft.drawString("Cached", width - 83, y, 2);
    drawStatusText(forecastError, width - inset, 14, true);
  }
  tft.setTextDatum(TL_DATUM);
}

void showSelectedRange() {
  rangeVisible = true;
  rangeShownAt = millis();
}

const char* formatSunCountdown(int minute, int sunrise, int sunset, char (&text)[32]) {
  if (sunrise < 0 || sunset <= sunrise) return "Sun unavailable";
  if (minute >= sunset) return "Night time";
  const bool beforeSunrise = minute < sunrise;
  const int remaining = (beforeSunrise ? sunrise : sunset) - minute;
  snprintf(text, sizeof(text), "%s in %02dh %02dm", beforeSunrise ? "Sunrise" : "Sunset",
           remaining / 60, remaining % 60);
  return text;
}

int drawFooterClock(time_t now) {
  constexpr int font = 2;
  constexpr int clockX = 0;
  constexpr int padding = 1;
  tm local = {};
  localtime_r(&now, &local);
  char clockTime[6];
  strftime(clockTime, sizeof(clockTime), "%H:%M", &local);
  clockPhase = millis() / clockHalfPeriodMs;
  const bool colonVisible = clockPhase % 2 == 0;
  clockTime[2] = '\0';
  const char* minutes = clockTime + 3;
  const int separatorX = padding + tft.textWidth(clockTime, font);
  const int colonX = separatorX + 1;
  const int separatorWidth = tft.textWidth(":", font) + 2;
  const int minutesX = separatorX + separatorWidth;
  const int clockWidth = minutesX + tft.textWidth(minutes, font) + padding;
  const int clockHeight = tft.fontHeight(font);
  TFT_eSprite mask(&tft);
  mask.setColorDepth(1);
  if (mask.createSprite(clockWidth, clockHeight)) {
    mask.setTextColor(TFT_WHITE, TFT_BLACK);
    mask.drawString(clockTime, padding, 0, font);
    mask.drawString(minutes, minutesX, 0, font);
    pushSmoothedMask(mask, clockX, footerY, TFT_WHITE, true);
  } else {
    tft.fillRect(clockX, footerY, clockWidth, clockHeight, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(clockTime, clockX + padding, footerY, font);
    tft.drawString(minutes, clockX + minutesX, footerY, font);
  }
  tft.fillRect(clockX + separatorX, footerY, separatorWidth, clockHeight, TFT_BLACK);
  if (colonVisible) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(":", clockX + colonX, footerY, font);
  }
  return clockX + clockWidth;
}

int drawFooterText(const char* text, int x, uint16_t color, uint8_t datum) {
  constexpr int baseline = 237;
  tft.loadFont(footerFont);
  tft.setTextDatum(datum);
  tft.setTextColor(color, TFT_BLACK);
  const int textWidth = tft.drawString(text, x, baseline);
  tft.unloadFont();
  tft.setTextDatum(TL_DATUM);
  return textWidth;
}

void drawSunCountdown(time_t now) {
  footerMinute = now / 60;
  tm local = {};
  localtime_r(&now, &local);
  char date[11];
  strftime(date, sizeof(date), "%Y-%m-%d", &local);

  int sunrise = -1, sunset = -1;
  for (int i = forecastOffset; i < forecastCount; ++i) {
    const auto& hour = snapshot.hours[i];
    if (strncmp(date, hour.time, 10) == 0 && hour.sunriseMinute >= 0) {
      sunrise = hour.sunriseMinute;
      sunset = hour.sunsetMinute;
      break;
    }
  }

  char text[32];
  const char* label = formatSunCountdown(local.tm_hour * 60 + local.tm_min, sunrise, sunset, text);
  tft.fillRect(footerDetailLeft, footerY, width - footerDetailLeft,
               tft.height() - footerY, TFT_BLACK);
  drawFooterText(label, footerRight, TFT_LIGHTGREY, R_BASELINE);
}

void updateRangeLabel() {
  if (rangeVisible) {
    if (millis() - rangeShownAt < rangeDisplayMs) return;
    rangeVisible = false;
    if (hasForecast) drawHeaderRight();
  }
  if (hasForecast) {
    const time_t now = time(nullptr);
    if (now / 60 != footerMinute) drawSunCountdown(now);
  }
}

void drawForecast() {
  const int visibleHours = shownHours();
  if (visibleHours <= 0) return;
  const WeatherHour* forecast = snapshot.hours + forecastOffset;
  const float currentTemperature = forecastOffset ? forecast[0].temperature : snapshot.currentTemperature;
  const uint16_t pink = tft.color565(255, 160, 158);
  const uint16_t blue = tft.color565(171, 216, 230);
  float low = forecast[0].temperature, high = low;
  for (int i = 0; i < visibleHours; ++i) {
    const auto& hour = forecast[i];
    low = min(low, hour.temperature);
    high = max(high, hour.temperature);
  }
  const float center = (low + high) / 2;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const int cityX = drawCurrentTemperature(currentTemperature) + 9;
  drawCityName(cityX);
  drawHeaderRight();

  const auto& currentHour = forecast[0];
  const char* conditions = comfortDescription(comfortLevel(currentHour, currentHour.precipitation));
  tft.loadFont(comfortFont);
  tft.setTextDatum(L_BASELINE);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(conditions, cityX, 37);
  tft.unloadFont();
  tft.setTextDatum(TL_DATUM);

  tft.startWrite();
  for (int i = 0; i < visibleHours; ++i) drawColumn(i);
  for (int i = 0; i < visibleHours; ++i) {
    if (i && !strncmp(forecast[i].time + 11, "00", 2)) {
      tft.drawFastVLine(column(i), top, bottom - top + 1, TFT_DARKGREY);
    }
  }
  // All strokes stay in their own hour column. Comfort, wind and temperature
  // are painted over the background layers in the order used by CyclingWeather.
  int previousComfort = 0, previousWind = 0, previousTemp = 0;
  for (int i = 0; i < visibleHours; ++i) {
    const auto& hour = forecast[i];
    const int comfort = graphY(comfortLevel(hour, hour.precipitation * 10));
    const int wind = graphY(hour.wind * 2.5f);
    const int temp = graphY(50 + 2 * (hour.temperature - center));
    drawStep(i, comfort, previousComfort, TFT_YELLOW, 2);
    drawStep(i, wind, previousWind, blue);
    drawStep(i, temp, previousTemp, pink);
    previousComfort = comfort;
    previousWind = wind;
    previousTemp = temp;
  }
  tft.endWrite();
  const int lastTempY = graphY(50 + 2 * (forecast[visibleHours - 1].temperature - center));
  Serial.printf("TFT temperature trace readback: %04x (expected %04x)\n",
                tft.readPixel(width - 1, lastTempY), pink);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  constexpr int axisFont = 1;
  int nextLabelX = 0;
  for (int i = 0; i < visibleHours; ++i) {
    const int x = column(i) + 2;
    if (x < nextLabelX) continue;
    const char* stamp = forecast[i].time;
    char label[8];
    if (i == 0 || !strncmp(stamp + 11, "00", 2)) {
      snprintf(label, sizeof(label), "%.2s.%.2s", stamp + 8, stamp + 5);
    } else if (!strncmp(stamp + 11, "12", 2)) {
      strcpy(label, "12:00");
    } else {
      continue;
    }
    if (x + tft.textWidth(label, axisFont) > width - 2) break;
    nextLabelX = x + tft.drawString(label, x, bottom + 3, axisFont) + 4;
  }
  char temperature[32];
  snprintf(temperature, sizeof(temperature), "Temp %+.0f..%+.0f C", low, high);
  const time_t now = time(nullptr);
  const int temperatureX = drawFooterClock(now) + 5;
  footerDetailLeft = temperatureX + drawFooterText(temperature, temperatureX, pink, L_BASELINE) + 4;

  if (rangeVisible) {
    footerMinute = -1;
    const char* terms[] = {"Wind", "Comfort", "Rain", "Snow"};
    const uint16_t colors[] = {blue, TFT_YELLOW, TFT_CYAN, TFT_WHITE};
    const int available = footerRight - footerDetailLeft;
    constexpr int font = 1;
    constexpr int y = footerY + 4;
    const int labelsWidth = tft.textWidth("WindComfortRainSnow", font);
    const int gap = constrain((available - labelsWidth) / 3, 3, 7);
    int x = footerRight - labelsWidth - 3 * gap;
    for (int i = 0; i < 4; ++i) {
      tft.setTextColor(colors[i], TFT_BLACK);
      x += tft.drawString(terms[i], x, y, font) + gap;
    }
  } else {
    drawSunCountdown(now);
  }
  Serial.printf("Rendered %d hours: %s to %s\n",
                visibleHours, forecast[0].time, forecast[visibleHours - 1].time);
}

void handleTouch() {
  static bool lastTouch = false;
  static bool pressed = false;
  static unsigned long changedAt = 0;
  const bool touching = touch.touched();
  const unsigned long now = millis();
  if (touching != lastTouch) {
    lastTouch = touching;
    changedAt = now;
  }
  if (touching == pressed || now - changedAt < 40) return;
  pressed = touching;
  if (pressed) {
    selectedDays = selectedDays % maxDays + 1;
    if (hasForecast) {
      showSelectedRange();
      drawForecast();
    }
  }
}

void drawStartupCredits() {
  tft.drawFastHLine(6, 79, width - 12, TFT_DARKGREY);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("https://weacodi.com", 6, 91, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Weather Comfort Diagram", 6, 119, 2);
  tft.drawString("Oleksii Zubovskyi & Olena Zubovska", 6, 138, 2);
  tft.drawString("Diagram license: CC BY-SA 4.0", 6, 157, 2);
  tft.drawString("DOI: 10.5281/zenodo.18916318", 6, 176, 2);
  tft.drawString("zubovsky@gmail.com", 6, 204, 2);
}

void showStatus(const char* status, bool error, const char* detail = nullptr,
                const char* explanation = nullptr) {
  Serial.println(status);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (error) drawCityName(6);
  else tft.drawString("Weacodi", 6, 5, 4);
  if (error) tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  drawStatusText(status, 6, 48);
  if (!error) drawStartupCredits();
  if (detail) {
    Serial.println(detail);
    drawStatusText(detail, 6, 67);
  }
  if (explanation) {
    Serial.println(explanation);
    drawStatusText(explanation, 6, 86);
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  touchSpi.begin(25, 39, 32, 33);
  touch.begin(touchSpi);
  if (!WIFI_SSID[0]) {
    showStatus("Wi-Fi not configured", true, "Set the network in wifi.local.h");
    return;
  }
  showStatus(wifiConnecting, false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(timezone, "pool.ntp.org", "time.cloudflare.com");
}

void useCachedForecast(const char* currentHour, const char* error, const char* detail = nullptr) {
  int offset = forecastOffset;
  while (currentHour && offset < forecastCount && strcmp(snapshot.hours[offset].time, currentHour) < 0) {
    ++offset;
  }
  if (offset == forecastCount) {
    hasForecast = false;
    forecastOffset = offset;
    if (forecastCount) showStatus("Cached forecast expired", true, error, detail);
    else showStatus(error, true, detail);
  } else if (forecastError != error || offset != forecastOffset) {
    forecastOffset = offset;
    forecastError = error;
    drawForecast();
  }
}

// Send the pixels read from the TFT controller as big-endian RGB565.
void dumpScreen() {
  uint16_t row[width];
  Serial.println("RGB565 320 240");
  for (int y = 0; y < tft.height(); ++y) {
    tft.readRect(0, y, width, 1, row);
    Serial.write(reinterpret_cast<const uint8_t*>(row), sizeof(row));
  }
}

void loop() {
  handleTouch();
  updateRangeLabel();
  if (hasForecast && millis() / clockHalfPeriodMs != clockPhase) {
    drawFooterClock(time(nullptr));
  }
  if (Serial.available() && Serial.read() == 's') dumpScreen();
  if (rangeVisible || !WIFI_SSID[0] || static_cast<int32_t>(millis() - nextRefresh) < 0) {
    delay(10);
    return;
  }
  tm local = {};
  const bool timeKnown = getLocalTime(&local, 0);
  char currentHour[17];
  if (timeKnown) strftime(currentHour, sizeof(currentHour), "%Y-%m-%dT%H:00", &local);
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool firstForecast = forecastCount == 0;
  if (firstForecast && wifiConnected != startupConnected) {
    startupConnected = wifiConnected;
    startupConnectedAt = millis();
    showStatus(wifiConnected ? wifiConnectedLabel : wifiConnecting, false);
  }
  if (!wifiConnected || !timeKnown) {
    const char* error = wifiConnected ? "Clock sync failed" : "Wi-Fi unavailable";
    if (waitingFor != error) {
      waitingFor = error;
      waitingSince = millis();
    }
    const bool waitExpired = millis() - waitingSince >= connectionGraceMs;
    if (hasForecast || waitExpired) {
      useCachedForecast(timeKnown ? currentHour : nullptr, error);
    }
    nextRefresh = millis() + (firstForecast && !waitExpired ? 250 : 5000);
    return;
  }
  waitingFor = nullptr;
  const ForecastRequestError error = fetchForecast(currentHour);
  if (firstForecast) {
    const unsigned long elapsed = millis() - startupConnectedAt;
    if (elapsed < connectedScreenMs) delay(connectedScreenMs - elapsed);
  }
  if (!error.message) {
    hasForecast = true;
    forecastError = nullptr;
    if (firstForecast) showSelectedRange();
    drawForecast();
    nextRefresh = millis() + 15 * 60 * 1000;
  } else {
    useCachedForecast(currentHour, error.message, error.detail);
    nextRefresh = millis() + 30000;
  }
}
