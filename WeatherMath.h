// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <Arduino.h>

struct WeatherHour {
  char time[17];
  float temperature, wind, humidity, cloud, precipitation, rain, snow, radiation;
  float sun;
  int16_t sunriseMinute = -1, sunsetMinute = -1;
  bool daylight, wet;
};

// Port of WeatherUtils for the diagram's normal sensitivity and intensity 0.
inline int comfortLevel(const WeatherHour& hour, float rain) {
  const float t = hour.temperature;
  const float wind = hour.wind;
  const float sun = hour.radiation;
  const float b = t > 0 ? 17.368f : 17.966f;
  const float c = t > 0 ? 238.88f : 247.15f;
  const float vapor = logf(constrain(hour.humidity, 1.f, 100.f) / 100.f) + b * t / (c + t);
  const float dew = c * vapor / (b - vapor);
  int score = 10;
  if (t < 5 || t > 35) score -= 4;
  else if (t < 15 || t >= 25) score -= 2;
  if (t < -6) score -= 3;
  else if (t < -3) score -= 2;
  else if (t < 0) score -= 1;
  if (t > 40) score -= 3;

  if (dew <= 10) score++;
  else if (dew > 25) score -= 5;
  else if (dew > 22) score -= 4;
  else if (dew > 19) score -= 3;
  else if (dew > 16) score -= 2;
  else if (dew > 13) score--;

  if (wind > 25) score -= 3;
  else if (wind >= 15) score--;
  else if (wind >= 5 && t > 25 && dew > 16) score++;

  if (rain > 10) score -= 5;
  else if (rain >= 5) score -= 3;
  else if (rain >= 1) score--;

  if (t < 10 && sun > 300) score += 2;
  else if (t < 20 && sun > 500) score++;
  else if (t > 28 && sun > 400) score -= 2;
  else if (t > 25 && sun > 300) score--;

  if (dew > 18 && sun > 400 && wind < 10 && t > 25) score--;
  if (dew > 21 && sun > 500 && wind < 5 && t > 28) score -= 2;
  if (dew > 22 && t > 30) score -= 2;
  else if (dew > 19 && t > 28) score--;
  return constrain(score, 1, 10) * 10;
}

inline const char* comfortDescription(int score) {
  if (score <= 20) return "Extreme conditions";
  if (score <= 50) return "Uncomfortable conditions";
  if (score <= 70) return "Moderate conditions";
  if (score <= 90) return "Comfortable conditions";
  return "Ideal conditions";
}

inline float dryingRate(float temperature, float wind) {
  constexpr float limits[] = {0, 5, 10, 15, 20};
  constexpr float rates[] = {0.025f, 0.06f, 0.11f, 0.18f, 0.25f, 0.34f};
  size_t band = 0;
  while (band < 5 && temperature > limits[band]) ++band;
  return rates[band] + constrain(wind, 0.f, 40.f) * 0.008f;
}

inline float liquidPrecipitation(float rain, float showers, float total) {
  const float liquid = fmaxf(0.f, rain) + fmaxf(0.f, showers);
  // The API can repeat the same liquid amount in both fields.
  return total > 0.f ? fminf(liquid, total) : liquid;
}

inline bool roadIsWet(float precipitation, float surfaceWater) {
  return precipitation >= 0.1f || surfaceWater >= 0.12f;
}

inline bool hasBlackIceRisk(const WeatherHour& hour) {
  return hour.wet && hour.temperature <= -1.f;
}
