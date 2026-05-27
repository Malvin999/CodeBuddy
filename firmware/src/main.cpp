#include "board_compat.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "about_info.h"
#include "clock_display_logic.h"
#include "clock_time_logic.h"
#include "data.h"
#include "persona_logic.h"
#include "utf8_text_logic.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// Advertise as "Codex-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Codex";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Codex-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 240, H = 135;
const uint8_t DISPLAY_ROTATION = 1;
const int SIDE_X = 96;
const int SIDE_W = W - SIDE_X;
const int SIDE_PAD = 4;
const int SIDE_TEXT_X = SIDE_X + SIDE_PAD;
const int SIDE_TEXT_CELLS = 22;
const int LEFT_CLOCK_Y = 76;
const int LED_PIN = 10;          // red LED, active-low

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background
const uint16_t USAGE_BLUE = 0x3D7F;
const uint16_t USAGE_LIME = 0xBFE0;
// Keep the minimal validation surface available for future bring-up, but run
// the normal UI path by default now that approval rendering is fixed.
static const bool VALIDATION_UI = false;

const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
uint32_t hudScrollStartedMs = 0;
char     lastPromptId[40] = "";
char     dismissedDoneSessionIds[8][16] = {};
uint8_t  dismissedDoneSessionNext = 0;
struct SessionStateMemo {
  char id[16];
  char state[12];
};
SessionStateMemo lastSessionStates[3] = {};
bool     lastSessionStatesReady = false;
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static uint8_t displayRotation() { return settings().clockRot == 1 ? 3 : DISPLAY_ROTATION; }
static void applyBrightness() { compatSetBrightnessPercent(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    compatSetDisplayEnabled(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

template <typename Canvas>
static void useDefaultTextFont(Canvas& canvas) {
  canvas.setFont(nullptr);
  canvas.setTextSize(1);
}

template <typename Canvas>
static void useUtf8FontForText(Canvas& canvas, const char* text, const lgfx::IFont* utf8Font) {
  if (text && utf8ContainsNonAscii(text)) {
    canvas.setFont(utf8Font);
    canvas.setTextSize(1);
  } else {
    canvas.setFont(nullptr);
    canvas.setTextSize(1);
  }
}

template <size_t N>
static void clipDisplayText(char (&out)[N], const char* text, uint8_t maxCells) {
  if (!text) {
    out[0] = 0;
    return;
  }
  size_t bytes = utf8ClipDisplayBytes(text, maxCells, N - 1);
  memcpy(out, text, bytes);
  out[bytes] = 0;
}

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = (W > H) || displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "rotation", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 2; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
  if (idx == 6) {
    M5.Lcd.setRotation(displayRotation());
    applyDisplayMode();
  }
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int rowH = H < 180 ? 10 : 14;
  int mw = W > H ? 196 : 118;
  int mh = 16 + SETTINGS_N * rowH + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * rowH);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 42, my + 8 + i * rowH);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "btnA", "usb" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: compatPowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

static bool    _clockHostTimeValid = false;
static int64_t _clockHostLocalEpoch = 0;
static uint32_t _clockHostSyncMs = 0;
// Cache the time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;

void clockOnTimeSync(int64_t local_epoch) {
  _clockHostTimeValid = true;
  _clockHostLocalEpoch = local_epoch;
  _clockHostSyncMs = millis();
}

static bool clockRefreshFromHostSync() {
  if (!_clockHostTimeValid) return false;
  int64_t local_epoch = _clockHostLocalEpoch + (int64_t)((millis() - _clockHostSyncMs) / 1000);
  ClockTimeFields fields = {};
  if (!clockFieldsFromLocalEpoch(local_epoch, &fields)) return false;
  _clkTm.Hours = fields.hours;
  _clkTm.Minutes = fields.minutes;
  _clkTm.Seconds = fields.seconds;
  _clkDt.year = fields.year;
  _clkDt.Month = fields.month;
  _clkDt.Date = fields.date;
  _clkDt.WeekDay = fields.week_day;
  return true;
}

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = compatVbusVoltageMv() > 4000;
  if (clockRefreshFromHostSync()) return;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; clockFormatHm(hm, sizeof(hm), _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; clockFormatSeconds(ss, sizeof(ss), _clkTm.Seconds);
  char wdl[16]; clockFormatWeekDateLine(wdl, sizeof(wdl), clockDow(), _clkDt.Month, _clkDt.Date);

  spr.fillRect(SIDE_X, 0, SIDE_W, H, p.bg);
  spr.drawFastVLine(SIDE_X, 8, H - 16, p.textDim);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.drawString(hm, SIDE_X + SIDE_W / 2, 38);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString(ss, SIDE_X + SIDE_W / 2, 68);
  spr.drawString(wdl, SIDE_X + SIDE_W / 2, 96);
  spr.setTextSize(1);
  spr.setTextColor(p.body, p.bg);
  spr.drawString("charging", SIDE_X + SIDE_W / 2, 120);
  spr.setTextDatum(TL_DATUM);
}

static void drawLeftClockPanel() {
  const Palette& p = characterPalette();
  char hm[6]; clockFormatHm(hm, sizeof(hm), _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; clockFormatSeconds(ss, sizeof(ss), _clkTm.Seconds);
  char wdl[16]; clockFormatWeekDateLine(wdl, sizeof(wdl), clockDow(), _clkDt.Month, _clkDt.Date);

  useDefaultTextFont(spr);
  spr.fillRect(0, LEFT_CLOCK_Y, SIDE_X, H - LEFT_CLOCK_Y, p.bg);
  spr.drawFastHLine(8, LEFT_CLOCK_Y, SIDE_X - 16, p.textDim);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(8, LEFT_CLOCK_Y + 8);
  spr.print(hm);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(72, LEFT_CLOCK_Y + 14);
  spr.print(ss);
  spr.setCursor(8, LEFT_CLOCK_Y + 33);
  spr.print(wdl);
  if (_onUsb) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(8, LEFT_CLOCK_Y + 47);
    spr.print("charging");
  }
}

PersonaState derive(const TamaState& s) {
  PersonaInputs input = {};
  input.connected = s.connected;
  input.sessionsRunning = s.sessionsRunning;
  input.sessionsWaiting = s.sessionsWaiting;
  return derivePersonaState(input);
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  useUtf8FontForText(spr, "Info", &fonts::efontCN_14);
  spr.setCursor(SIDE_TEXT_X, y); spr.print("Info");
  useDefaultTextFont(spr);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  useUtf8FontForText(spr, section, &fonts::efontCN_14);
  spr.setCursor(SIDE_TEXT_X, y); spr.print(section);
  y += utf8ContainsNonAscii(section) ? 14 : 12;
  useDefaultTextFont(spr);
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(12, 22);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(12, 98); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 58);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  spr.fillRect(SIDE_X, 0, SIDE_W, H, p.bg);
  spr.drawFastVLine(SIDE_X, 8, H - 16, p.textDim);
  spr.setTextSize(1);
  int y = 4;
  auto ln = [&](const char* fmt, ...) {
    char b[80]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    utf8TrimIncompleteTail(b);
    char line[80];
    clipDisplayText(line, b, SIDE_TEXT_CELLS);
    useUtf8FontForText(spr, line, &fonts::efontCN_10);
    spr.setCursor(SIDE_TEXT_X, y); spr.print(line);
    y += utf8ContainsNonAscii(line) ? 11 : 8;
    useDefaultTextFont(spr);
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Codex");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CODEX", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "open");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = compatBatteryVoltageMv();
    int iBat_mA = compatBatteryCurrentMa();
    int vBus_mV = compatVbusVoltageMv();
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(SIDE_TEXT_X, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(SIDE_TEXT_X + 56, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  ble      %s", dataBtActive() ? "linked" : "discover");
    ln("  temp     n/a");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = dataBtActive();

    spr.setTextColor(linked ? GREEN : HOT, p.bg);
    spr.setTextSize(2);
    spr.setCursor(SIDE_TEXT_X, y);
    spr.print(linked ? "linked" : "discover");
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" code-buddy");
      y += 4;
      ln("TO STAY LINKED");
      ln(" run codex");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    AboutInfo about = currentAboutInfo();
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("%s", about.made_by);
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("%s", about.source_line_1);
    ln("%s", about.source_line_2);
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("%s", about.hardware_line_1);
    ln("%s", about.hardware_line_2);
  }
}
static void drawApproval() {
  const Palette& p = characterPalette();
  const int X = SIDE_X;
  const int AREA = SIDE_W;
  spr.fillRect(X, 0, AREA, H, p.bg);
  spr.drawFastVLine(X, 8, H - 16, p.textDim);

  useDefaultTextFont(spr);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(X + SIDE_PAD, 6);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  char toolLine[48];
  bool toolUtf8 = utf8ContainsNonAscii(tama.promptTool);
  uint16_t toolCells = utf8DisplayCells(tama.promptTool);
  clipDisplayText(toolLine, tama.promptTool, toolUtf8 ? 10 : (toolCells <= 10 ? 10 : SIDE_TEXT_CELLS));
  spr.setTextColor(p.text, p.bg);
  if (toolUtf8) {
    spr.setFont(&fonts::efontCN_14);
    spr.setTextSize(1);
    spr.setCursor(X + SIDE_PAD, 24);
  } else {
    useDefaultTextFont(spr);
    spr.setTextSize(toolCells <= 8 ? 2 : 1);
    spr.setCursor(X + SIDE_PAD, toolCells <= 8 ? 22 : 26);
  }
  spr.print(toolLine);
  useDefaultTextFont(spr);

  // Hint wraps by display cells so multi-byte UTF-8 never gets split.
  char hintLines[8][48] = {};
  uint8_t hintRows = utf8WrapInto(tama.promptHint, hintLines, 8, SIDE_TEXT_CELLS, false);
  uint8_t hintBack = (hintRows > 4) ? (hintRows - 4) : 0;
  uint8_t hintOffset = utf8AutoScrollOffset(hintBack, millis() - promptArrivedMs);
  spr.setTextColor(p.textDim, p.bg);
  for (uint8_t i = 0; i < 4 && (hintOffset + i) < hintRows; ++i) {
    useUtf8FontForText(spr, hintLines[hintOffset + i], &fonts::efontCN_12);
    spr.setCursor(X + SIDE_PAD, 48 + i * 12);
    spr.print(hintLines[hintOffset + i]);
  }
  useDefaultTextFont(spr);

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(X + SIDE_PAD, H - 14);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(X + SIDE_PAD, H - 14);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 44, H - 14);
    spr.print("B: deny");
  }
}

static void drawValidationScreen(bool inPrompt) {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, 8);
  spr.print("CodeBuddy BLE");
  spr.setTextColor((millis() / 500) % 2 ? GREEN : p.textDim, p.bg);
  spr.setCursor(W - 18, 8);
  spr.print("o");

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, 24);
  spr.printf("peer    %s", tama.connected ? "live" : "idle");
  spr.setCursor(6, 36);
  spr.printf("waiting %u", tama.sessionsWaiting);
  spr.setCursor(6, 48);
  spr.printf("prompt  %s", tama.promptId[0] ? "yes" : "no");
  spr.setCursor(6, 60);
  spr.print("msg ");
  char msgLine[32];
  clipDisplayText(msgLine, tama.msg, 16);
  useUtf8FontForText(spr, msgLine, &fonts::efontCN_12);
  spr.print(msgLine);
  useDefaultTextFont(spr);

  if (inPrompt) {
    drawApproval();
  } else {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(6, 92);
    spr.print("waiting for prompt");
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(6, 106);
    spr.print("host link only mode");
  }

  spr.pushSprite(0, 0);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  spr.fillRect(SIDE_X, 0, SIDE_W, H, p.bg);
  spr.drawFastVLine(SIDE_X, 8, H - 16, p.textDim);
  spr.setTextSize(1);
  int y = 24;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SIDE_TEXT_X, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(SIDE_TEXT_X + 48 + i * 14, y + 2, i < mood, moodCol);

  y += 17;
  spr.setCursor(SIDE_TEXT_X, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = SIDE_TEXT_X + 34 + i * 8;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 17;
  spr.setCursor(SIDE_TEXT_X, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = SIDE_TEXT_X + 54 + i * 11;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 18;
  spr.fillRoundRect(SIDE_TEXT_X, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(SIDE_TEXT_X + 5, y + 1); spr.printf("Lv %u", stats().level);

  y += 18;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SIDE_TEXT_X, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(SIDE_TEXT_X, y + 10);
  spr.printf("denied   %u", stats().denials);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(SIDE_TEXT_X, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 20);
  tokFmt("today    ", tama.tokensToday, y + 30);
}

static void drawPetHowTo(const Palette& p) {
  spr.fillRect(SIDE_X, 0, SIDE_W, H, p.bg);
  spr.drawFastVLine(SIDE_X, 8, H - 16, p.textDim);
  spr.setTextSize(1);
  int y = 18;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(SIDE_TEXT_X, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 2;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(SIDE_TEXT_X, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

static uint8_t pctFromCap(uint32_t value, uint32_t cap) {
  if (cap == 0) return 0;
  uint32_t pct = (uint32_t)((uint64_t)value * 100 / cap);
  return (uint8_t)(pct > 100 ? 100 : pct);
}

static uint8_t batteryPercent() {
  int pct = (compatBatteryVoltageMv() - 3200) / 10;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (uint8_t)pct;
}

static void drawBatteryPercent(const Palette& p) {
  char label[6];
  snprintf(label, sizeof(label), "%u%%", batteryPercent());
  spr.setTextSize(1);
  spr.setTextColor(_onUsb ? USAGE_LIME : p.textDim, p.bg);
  spr.setCursor(W - (int)strlen(label) * 6 - 5, 5);
  spr.print(label);
}

static void formatCompactNumber(char* out, size_t size, uint32_t value) {
  if (value >= 1000000) {
    snprintf(out, size, "%lu.%luM", (unsigned long)(value / 1000000), (unsigned long)((value / 100000) % 10));
  } else if (value >= 1000) {
    snprintf(out, size, "%lu.%luK", (unsigned long)(value / 1000), (unsigned long)((value / 100) % 10));
  } else {
    snprintf(out, size, "%lu", (unsigned long)value);
  }
}

static void drawUsageBar(int x, int y, int w, int h, uint8_t pct, uint16_t fill, uint16_t border) {
  spr.drawRect(x, y, w, h, border);
  int inner = w - 2;
  int filled = (int)((uint32_t)inner * pct / 100);
  if (filled > 0) spr.fillRect(x + 1, y + 1, filled, h - 2, fill);
}

static void drawUsageRow(const Palette& p, int y, uint8_t pct, const char* window,
                         const char* detail, uint16_t color) {
  char pctLine[8];
  snprintf(pctLine, sizeof(pctLine), "%u%%", pct);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(2);
  spr.setCursor(SIDE_TEXT_X, y);
  spr.print(pctLine);
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  int winX = W - (int)strlen(window) * 12 - 6;
  if (winX < SIDE_TEXT_X + 70) winX = SIDE_TEXT_X + 70;
  spr.setTextSize(2);
  spr.setCursor(winX, y);
  spr.print(window);
  spr.setTextSize(1);

  drawUsageBar(SIDE_TEXT_X, y + 23, SIDE_W - SIDE_PAD * 2, 11, pct, color, p.textDim);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(SIDE_TEXT_X, y + 38);
  spr.print(detail);
}

static void drawUsagePanel() {
  const Palette& p = characterPalette();
  spr.fillRect(SIDE_X, 0, SIDE_W, H, p.bg);
  spr.drawFastVLine(SIDE_X, 8, H - 16, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(SIDE_TEXT_X, 5);
  spr.print("CODEX USAGE");
  drawBatteryPercent(p);

  uint8_t shortPct = tama.usageQuota ? tama.usageShortPct : pctFromCap(tama.tokensToday, 100000);
  uint8_t longPct = tama.usageQuota
      ? tama.usageLongPct
      : pctFromCap(stats().tokens % TOKENS_PER_LEVEL, TOKENS_PER_LEVEL);
  const char* shortWindow = (tama.usageQuota && tama.usageShortWindow[0]) ? tama.usageShortWindow : "today";
  const char* longWindow = (tama.usageQuota && tama.usageLongWindow[0]) ? tama.usageLongWindow : "level";

  char shortDetail[32];
  char longDetail[32];
  if (tama.usageQuota) {
    snprintf(shortDetail, sizeof(shortDetail), "resets in %s", tama.usageShortReset[0] ? tama.usageShortReset : "-");
    snprintf(longDetail, sizeof(longDetail), "resets in %s", tama.usageLongReset[0] ? tama.usageLongReset : "-");
  } else {
    char today[12], total[12];
    formatCompactNumber(today, sizeof(today), tama.tokensToday);
    formatCompactNumber(total, sizeof(total), stats().tokens);
    snprintf(shortDetail, sizeof(shortDetail), "%s output tokens", today);
    snprintf(longDetail, sizeof(longDetail), "%s lifetime tokens", total);
  }

  drawUsageRow(p, 24, shortPct, shortWindow, shortDetail, USAGE_BLUE);
  drawUsageRow(p, 78, longPct, longWindow, longDetail, USAGE_LIME);
}

static uint16_t sessionColor(const char* state) {
  if (strcmp(state, "running") == 0) return USAGE_BLUE;
  if (strcmp(state, "waiting") == 0) return HOT;
  return USAGE_LIME;
}

static const char* sessionLabel(const char* state) {
  if (strcmp(state, "running") == 0) return "work";
  if (strcmp(state, "waiting") == 0) return "approve";
  return "done";
}

static bool sessionIsWaiting(const char* state) {
  return strcmp(state, "waiting") == 0;
}

static bool sessionIsDone(const char* state) {
  return strcmp(state, "done") == 0;
}

static const char* previousSessionState(const char* id) {
  if (!id || !id[0]) return nullptr;
  for (uint8_t i = 0; i < 3; ++i) {
    if (strcmp(lastSessionStates[i].id, id) == 0) return lastSessionStates[i].state;
  }
  return nullptr;
}

static void rememberCurrentSessionStates() {
  memset(lastSessionStates, 0, sizeof(lastSessionStates));
  for (uint8_t i = 0; i < tama.sessionCount && i < 3; ++i) {
    strncpy(lastSessionStates[i].id, tama.sessions[i].id, sizeof(lastSessionStates[i].id) - 1);
    lastSessionStates[i].id[sizeof(lastSessionStates[i].id) - 1] = 0;
    strncpy(lastSessionStates[i].state, tama.sessions[i].state, sizeof(lastSessionStates[i].state) - 1);
    lastSessionStates[i].state[sizeof(lastSessionStates[i].state) - 1] = 0;
  }
}

static void focusSessionPanel() {
  napping = false;
  wake();
  displayMode = DISP_NORMAL;
  menuOpen = settingsOpen = resetOpen = false;
  applyDisplayMode();
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

static void handleSessionTransitionAlerts(bool suppressSessionAudio) {
  if (!dataConnected()) {
    lastSessionStatesReady = false;
    memset(lastSessionStates, 0, sizeof(lastSessionStates));
    return;
  }

  bool becameWaiting = false;
  bool becameDone = false;
  if (lastSessionStatesReady) {
    for (uint8_t i = 0; i < tama.sessionCount && i < 3; ++i) {
      const char* id = tama.sessions[i].id;
      const char* state = tama.sessions[i].state;
      const char* previous = previousSessionState(id);
      if (!previous || strcmp(previous, state) == 0) continue;
      if (sessionIsWaiting(state)) becameWaiting = true;
      else if (sessionIsDone(state)) becameDone = true;
    }
  }

  rememberCurrentSessionStates();
  lastSessionStatesReady = true;

  if (suppressSessionAudio) return;
  if (becameWaiting) {
    focusSessionPanel();
    beep(1200, 80);
  } else if (becameDone) {
    beep(2200, 50);
  }
}

static bool dismissedDoneSession(const char* id) {
  if (!id || !id[0]) return false;
  for (uint8_t i = 0; i < 8; ++i) {
    if (strcmp(dismissedDoneSessionIds[i], id) == 0) return true;
  }
  return false;
}

static void rememberDismissedDoneSession(const char* id) {
  if (!id || !id[0] || dismissedDoneSession(id)) return;
  strncpy(dismissedDoneSessionIds[dismissedDoneSessionNext], id,
          sizeof(dismissedDoneSessionIds[dismissedDoneSessionNext]) - 1);
  dismissedDoneSessionIds[dismissedDoneSessionNext][sizeof(dismissedDoneSessionIds[dismissedDoneSessionNext]) - 1] = 0;
  dismissedDoneSessionNext = (dismissedDoneSessionNext + 1) % 8;
}

static void restoreDismissedDoneSession(const char* id) {
  if (!id || !id[0]) return;
  for (uint8_t i = 0; i < 8; ++i) {
    if (strcmp(dismissedDoneSessionIds[i], id) == 0) {
      dismissedDoneSessionIds[i][0] = 0;
    }
  }
}

static void restoreActiveSessionDismissals() {
  for (uint8_t i = 0; i < tama.sessionCount && i < 3; ++i) {
    if (!sessionIsDone(tama.sessions[i].state)) {
      restoreDismissedDoneSession(tama.sessions[i].id);
    }
  }
}

static bool sessionVisible(uint8_t idx) {
  if (idx >= tama.sessionCount || idx >= 3) return false;
  return !(sessionIsDone(tama.sessions[idx].state) && dismissedDoneSession(tama.sessions[idx].id));
}

static uint8_t visibleSessionCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < tama.sessionCount && i < 3; ++i) {
    if (sessionVisible(i)) n++;
  }
  return n;
}

static bool dismissVisibleDoneSessions() {
  bool dismissed = false;
  for (uint8_t i = 0; i < tama.sessionCount && i < 3; ++i) {
    if (sessionVisible(i) && sessionIsDone(tama.sessions[i].state)) {
      rememberDismissedDoneSession(tama.sessions[i].id);
      dismissed = true;
    }
  }
  return dismissed;
}

static void drawSessionIcon(int x, int y, const char* state, uint16_t color, const Palette& p) {
  if (strcmp(state, "running") == 0) {
    spr.fillCircle(x, y, 4, color);
    spr.drawFastHLine(x - 2, y, 5, p.bg);
    return;
  }
  if (strcmp(state, "waiting") == 0) {
    spr.fillTriangle(x, y - 5, x - 5, y + 4, x + 5, y + 4, color);
    spr.setTextColor(p.bg, color);
    spr.setCursor(x - 1, y - 2);
    spr.print("!");
    return;
  }
  spr.drawLine(x - 5, y, x - 1, y + 4, color);
  spr.drawLine(x - 1, y + 4, x + 6, y - 5, color);
}

static void drawSessionPanel() {
  const Palette& p = characterPalette();
  spr.fillRect(SIDE_X, 0, SIDE_W, H, p.bg);
  spr.drawFastVLine(SIDE_X, 8, H - 16, p.textDim);
  useDefaultTextFont(spr);
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(SIDE_TEXT_X, 5);
  spr.print("CODEX SESSIONS");
  drawBatteryPercent(p);

  uint8_t visibleIdx = 0;
  uint8_t visibleTotal = visibleSessionCount();
  for (uint8_t i = 0; i < tama.sessionCount && i < 3; ++i) {
    if (!sessionVisible(i)) continue;
    const int y = 25 + visibleIdx * 36;
    const char* state = tama.sessions[i].state;
    uint16_t color = sessionColor(state);
    drawSessionIcon(SIDE_TEXT_X + 6, y + 9, state, color, p);

    char title[48];
    clipDisplayText(title, tama.sessions[i].name, 18);
    spr.setTextColor(p.text, p.bg);
    useUtf8FontForText(spr, title, &fonts::efontCN_12);
    spr.setCursor(SIDE_TEXT_X + 18, y);
    spr.print(title);
    useDefaultTextFont(spr);

    spr.setTextColor(color, p.bg);
    spr.setCursor(SIDE_TEXT_X + 18, y + 15);
    spr.print(sessionLabel(state));

    if (visibleIdx < visibleTotal - 1 && visibleIdx < 2) {
      spr.drawFastHLine(SIDE_TEXT_X, y + 29, SIDE_W - SIDE_PAD * 2, p.textDim);
    }
    visibleIdx++;
  }
}

void drawHUD() {
  if (visibleSessionCount() > 0) { drawSessionPanel(); return; }
  drawUsagePanel();
}

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;
  cfg.internal_imu = true;
  cfg.internal_spk = true;
  cfg.internal_rtc = true;
  M5.begin(cfg);
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 500) delay(10);
  Serial.println("[boot] setup");
  M5.Lcd.setRotation(DISPLAY_ROTATION);
  M5.Imu.Init();
  M5.Beep.begin();
  startBt();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  M5.Lcd.setRotation(displayRotation());
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.createSprite(W, H);
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  restoreActiveSessionDismissals();
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: beep, reset response flag
  bool promptAlertPlayed = false;
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      Serial.printf(
        "[prompt] show id=%s tool=%s hud=%d mode=%u\n",
        tama.promptId, tama.promptTool, settings().hud ? 1 : 0, displayMode
      );
      promptArrivedMs = millis();
      napping = false;
      wake();
      beep(1200, 80);   // alert chirp
      promptAlertPlayed = true;
      // Jump home so the read-only session list can show the waiting state.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }
  handleSessionTransitionAlerts(promptAlertPlayed);
  if (screenOff && visibleSessionCount() > 0) {
    wake();
  }

  bool inPrompt = false;

  if (VALIDATION_UI) {
    napping = false;
    if (screenOff) {
      compatSetDisplayEnabled(true);
      screenOff = false;
    }

    if (M5.BtnA.wasReleased() && inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      Serial.printf("[prompt] approve id=%s\n", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
      inPrompt = false;
    }

    if (M5.BtnB.wasReleased() && inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      Serial.printf("[prompt] deny id=%s\n", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
      inPrompt = false;
    }

    drawValidationScreen(inPrompt);
    delay(16);
    return;
  }

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (compatPowerKeyState() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      compatSetDisplayEnabled(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        Serial.printf("[prompt] approve id=%s\n", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      Serial.printf("[prompt] deny id=%s\n", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else if (visibleSessionCount() > 0 && dismissVisibleDoneSessions()) {
      beep(1400, 30);
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Codex data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  static bool wasClocking = false;
  if (clocking != wasClocking) {
    applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff) {
    // skip sprite render while face-down or powered off
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else {
      if (buddyMode || characterLoaded()) drawLeftClockPanel();
      if (displayMode == DISP_INFO) drawInfo();
      else if (displayMode == DISP_PET) drawPet();
      else drawHUD();
      if (resetOpen) drawReset();
      else if (settingsOpen) drawSettings();
      else if (menuOpen) drawMenu();
    }
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    compatSetBrightnessPercent(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // Keep live sessions visible. Once the session list ages out and the home
  // surface falls back to usage, non-USB power can auto-off again.
  if (!screenOff && !inPrompt && !_onUsb && visibleSessionCount() == 0
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    compatSetDisplayEnabled(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
