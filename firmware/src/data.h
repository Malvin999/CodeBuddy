#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "utf8_text_logic.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  uint8_t  sessionCount;
  struct {
    char id[16];
    char name[64];
    char state[12];
  } sessions[3];
  bool     recentlyCompleted;
  uint32_t tokensTotal;
  uint32_t tokensToday;
  uint32_t tokensActive;
  bool     usageLive;
  bool     usageQuota;
  uint8_t  usageShortPct;
  uint8_t  usageLongPct;
  char     usageShortWindow[8];
  char     usageLongWindow[8];
  char     usageShortReset[24];
  char     usageLongReset[24];
  char     presenceState[12];
  uint32_t presenceIdleSec;
  bool     presenceWork;
  uint32_t lastUpdated;
  char     msg[128];
  bool     connected;
  char     lines[8][256];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[96];
  char     promptHint[256];
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Codex connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.printf("[data] json err=%s head=%.48s\n", err.c_str(), line);
    return;
  }
  if (xferCommand(doc)) {
    const char* cmd = doc["cmd"];
    Serial.printf("[data] cmd=%s\n", cmd ? cmd : "(null)");
    _lastLiveMs = millis();
    return;
  }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    RTC_TimeTypeDef tm((int8_t)lt.tm_hour, (int8_t)lt.tm_min, (int8_t)lt.tm_sec);
    RTC_DateTypeDef dt((int16_t)(lt.tm_year + 1900), (int8_t)(lt.tm_mon + 1),
                       (int8_t)lt.tm_mday, (int8_t)lt.tm_wday);
    M5.Rtc.SetTime(&tm);
    M5.Rtc.SetDate(&dt);
    extern uint32_t _clkLastRead;
    extern void clockOnTimeSync(int64_t local_epoch);
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    clockOnTimeSync((int64_t)local);
    _rtcValid = true;
    Serial.println("[data] time sync");
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) {
    out->tokensTotal = bridgeTokens;
    statsOnBridgeTokens(bridgeTokens);
  }
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  out->tokensActive = doc["tokens_active"] | out->tokensActive;
  JsonObject usage = doc["usage"];
  if (!usage.isNull()) {
    int shortPct = usage["short_pct"] | 0;
    int longPct = usage["long_pct"] | 0;
    if (shortPct < 0) shortPct = 0;
    if (shortPct > 100) shortPct = 100;
    if (longPct < 0) longPct = 0;
    if (longPct > 100) longPct = 100;
    out->usageQuota = true;
    out->usageLive = usage["live"] | true;
    out->usageShortPct = (uint8_t)shortPct;
    out->usageLongPct = (uint8_t)longPct;
    const char* shortWindow = usage["short_window"] | "5h";
    const char* longWindow = usage["long_window"] | "7d";
    const char* shortReset = usage["short_reset"] | "";
    const char* longReset = usage["long_reset"] | "";
    strncpy(out->usageShortWindow, shortWindow, sizeof(out->usageShortWindow) - 1);
    strncpy(out->usageLongWindow, longWindow, sizeof(out->usageLongWindow) - 1);
    strncpy(out->usageShortReset, shortReset, sizeof(out->usageShortReset) - 1);
    strncpy(out->usageLongReset, longReset, sizeof(out->usageLongReset) - 1);
    out->usageShortWindow[sizeof(out->usageShortWindow) - 1] = 0;
    out->usageLongWindow[sizeof(out->usageLongWindow) - 1] = 0;
    out->usageShortReset[sizeof(out->usageShortReset) - 1] = 0;
    out->usageLongReset[sizeof(out->usageLongReset) - 1] = 0;
  }
  JsonObject presence = doc["presence"];
  if (!presence.isNull()) {
    const char* state = presence["state"] | "";
    strncpy(out->presenceState, state, sizeof(out->presenceState) - 1);
    out->presenceState[sizeof(out->presenceState) - 1] = 0;
    out->presenceIdleSec = presence["idle_sec"] | out->presenceIdleSec;
    out->presenceWork = presence["work"] | out->presenceWork;
  }
  JsonArray sessions = doc["sessions"];
  if (!sessions.isNull()) {
    out->sessionCount = 0;
    for (JsonVariant v : sessions) {
      if (out->sessionCount >= 3) break;
      JsonObject session = v.as<JsonObject>();
      if (session.isNull()) continue;
      const char* id = session["id"] | "";
      const char* name = session["name"] | "";
      const char* state = session["state"] | "done";
      strncpy(out->sessions[out->sessionCount].id, id, sizeof(out->sessions[out->sessionCount].id) - 1);
      out->sessions[out->sessionCount].id[sizeof(out->sessions[out->sessionCount].id) - 1] = 0;
      utf8CopyTruncate(out->sessions[out->sessionCount].name, name);
      strncpy(out->sessions[out->sessionCount].state, state, sizeof(out->sessions[out->sessionCount].state) - 1);
      out->sessions[out->sessionCount].state[sizeof(out->sessions[out->sessionCount].state) - 1] = 0;
      out->sessionCount++;
    }
  }
  const char* m = doc["msg"];
  if (m) utf8CopyTruncate(out->msg, m);
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      utf8CopyTruncate(out->lines[n], s ? s : "");
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    utf8CopyTruncate(out->promptTool, pt ? pt : "");
    utf8CopyTruncate(out->promptHint, ph ? ph : "");
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  Serial.printf(
    "[data] snapshot total=%u running=%u waiting=%u prompt=%s msg=%.36s\n",
    out->sessionsTotal,
    out->sessionsRunning,
    out->sessionsWaiting,
    out->promptId[0] ? out->promptId : "-",
    out->msg
  );
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensTotal=s.tok; out->tokensToday=s.tok; out->tokensActive=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->sessionCount=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    utf8CopyTruncate(out->msg, "No Codex connected");
  }
}
