#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#define SCREEN_W   128
#define SCREEN_H   64
#define OLED_ADDR  0x3C

#define PIN_SDA     21
#define PIN_SCL     22
#define PIN_BTN      4
#define PIN_VIN     34
#define PIN_NODE    32
#define PIN_REF10K  33
#define PIN_REF1K   25
#define PIN_BUZZER  26
#define PIN_LED      2

#define DEBUG_SERIAL 1

const float R_TOP         = 82000.0f;
const float R_BOTTOM      = 20000.0f;
const float V_CAL         = 1.000f;
const float PIN_OFFSET_MV = 0.0f;
const float V_DEADBAND    = 0.90f;
const float R_HIGH        = 10000.0f;
const float R_LOW         = 1000.0f;
const float VCC_MV        = 3300.0f;
const float SHORT_MV      = 100.0f;
const float CONT_OHMS     = 50.0f;

const char U_V[]    = "V";
const char U_OHM[]  = "\xEA";
const char U_KOHM[] = "k\xEA";
const char U_MOHM[] = "M\xEA";

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

enum Mode { MODE_VOLT, MODE_OHM, MODE_LED, MODE_COUNT };
enum { OHM_OPEN, OHM_SHORT, OHM_VALUE };
struct OhmResult { uint8_t state; float ohms; };

int      mode     = MODE_VOLT;
float    vFilt    = -1, vShown = 0, rShown = 0, lFilt = -1, barAnim = 0;
uint32_t lastDraw = 0;

bool buttonPressed() {
  static bool last = HIGH;
  static uint32_t t = 0;
  bool now = digitalRead(PIN_BTN);
  bool fired = (last == HIGH && now == LOW && millis() - t > 200);
  if (fired) t = millis();
  last = now;
  return fired;
}

void setBeep(bool on) {
  digitalWrite(PIN_BUZZER, on);
  digitalWrite(PIN_LED, on);
}

void refsOff() {
  pinMode(PIN_REF10K, INPUT);
  pinMode(PIN_REF1K, INPUT);
}

float readMv(uint8_t pin, uint8_t n) {
  uint16_t s[64];
  if (n > 64) n = 64;
  analogReadMilliVolts(pin);
  analogReadMilliVolts(pin);
  for (uint8_t i = 0; i < n; i++) s[i] = (uint16_t)analogReadMilliVolts(pin);
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; }
    s[j + 1] = k;
  }
  uint8_t lo = n / 4, hi = n - n / 4;
  uint32_t sum = 0;
  for (uint8_t i = lo; i < hi; i++) sum += s[i];
  return sum / (float)(hi - lo);
}

void drawHeader(const char* title) {
  display.fillRect(0, 0, SCREEN_W, 12, SSD1306_WHITE);
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(4, 2);
  display.print(title);
  for (int i = 0; i < MODE_COUNT; i++) {
    int cx = 104 + i * 9;
    if (i == mode) display.fillCircle(cx, 5, 2, SSD1306_BLACK);
    else           display.drawCircle(cx, 5, 2, SSD1306_BLACK);
  }
}

void footerText(const char* left, const char* right) {
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 56);
  display.print(left);
  display.setCursor(124 - (int)strlen(right) * 6, 56);
  display.print(right);
}

void drawSegBar(float frac, int y) {
  frac = constrain(frac, 0.0f, 1.0f);
  int on = (int)(frac * 20.0f + 0.5f);
  for (int i = 0; i < 20; i++) {
    int x = 4 + i * 6;
    if (i < on) display.fillRect(x, y, 5, 5, SSD1306_WHITE);
    else        display.drawFastHLine(x, y + 2, 5, SSD1306_WHITE);
  }
}

void drawSegBarAnim(float target, int y) {
  target = constrain(target, 0.0f, 1.0f);
  barAnim += (target - barAnim) * 0.35f;
  drawSegBar(barAnim, y);
}

const GFXfont* chooseFont(const char* s, int maxW, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
  const GFXfont* fonts[3] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
  for (int i = 0; i < 3; i++) {
    display.setFont(fonts[i]);
    display.setTextSize(1);
    display.getTextBounds(s, 0, 0, x1, y1, w, h);
    if ((int)*w <= maxW) return fonts[i];
  }
  return fonts[2];
}

void drawValue(const char* val, const char* unit, int cy) {
  int unitW = (int)strlen(unit) * 12;
  int reserve = unitW ? unitW + 4 : 0;
  int16_t x1, y1, rx, ry;
  uint16_t w, h, rw, rh;
  const GFXfont* f = chooseFont(val, 122 - reserve, &x1, &y1, &w, &h);
  display.setFont(f);
  display.getTextBounds("0", 0, 0, &rx, &ry, &rw, &rh);
  int baseY  = cy - (ry + (int)rh / 2);
  int startX = (SCREEN_W - ((int)w + reserve)) / 2;
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(startX - x1, baseY);
  display.print(val);
  if (unitW) {
    display.setFont();
    display.setTextSize(2);
    display.setCursor(startX + (int)w + 4, baseY - 14);
    display.print(unit);
  }
  display.setFont();
  display.setTextSize(1);
}

void drawFitLeft(const char* s, int x, int cy, int maxW) {
  int16_t x1, y1, rx, ry;
  uint16_t w, h, rw, rh;
  const GFXfont* f = chooseFont(s, maxW, &x1, &y1, &w, &h);
  display.setFont(f);
  display.getTextBounds("0", 0, 0, &rx, &ry, &rw, &rh);
  int baseY = cy - (ry + (int)rh / 2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x - x1, baseY);
  display.print(s);
  display.setFont();
  display.setTextSize(1);
}

void drawSmall(const char* s, int x, int y) {
  display.setFont();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(s);
}

void drawLedIcon(bool good, bool raysOn) {
  display.drawFastHLine(2, 34, 8, SSD1306_WHITE);
  display.drawFastHLine(30, 34, 8, SSD1306_WHITE);
  if (good) display.fillTriangle(10, 22, 10, 46, 30, 34, SSD1306_WHITE);
  else      display.drawTriangle(10, 22, 10, 46, 30, 34, SSD1306_WHITE);
  display.drawFastVLine(30, 22, 25, SSD1306_WHITE);
  if (good && raysOn) {
    display.drawLine(24, 19, 31, 12, SSD1306_WHITE);
    display.drawLine(30, 23, 37, 16, SSD1306_WHITE);
  }
}

void wipe() {
  for (int x = 0; x <= SCREEN_W; x += 32) {
    display.fillRect(0, 0, x, SCREEN_H, SSD1306_WHITE);
    display.display();
  }
  for (int x = 0; x <= SCREEN_W; x += 32) {
    display.fillRect(0, 0, x, SCREEN_H, SSD1306_BLACK);
    display.display();
  }
}

void drawBolt() {
  display.fillTriangle(72, 6, 54, 34, 66, 34, SSD1306_WHITE);
  display.fillTriangle(58, 58, 76, 30, 62, 30, SSD1306_WHITE);
}

void drawBrandScene(int blockX, int tagOff) {
  display.clearDisplay();
  display.setFont();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 2);
  display.print("TechTadka");
  display.fillRoundRect(blockX, 24, 44, 18, 3, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(blockX + 6, 27);
  display.print("360");
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(58, 24 + tagOff);
  display.print("SMART");
  display.setCursor(58, 34 + tagOff);
  display.print("MULTIMETER");
}

void bootAnimation() {
  for (int i = 0; i <= 62; i += 3) {
    display.clearDisplay();
    display.drawFastHLine(0, 32, i, SSD1306_WHITE);
    display.drawFastHLine(127 - i, 32, i, SSD1306_WHITE);
    display.fillCircle(i, 32, 3, SSD1306_WHITE);
    display.fillCircle(127 - i, 32, 3, SSD1306_WHITE);
    display.display();
  }
  display.invertDisplay(true);
  delay(70);
  display.invertDisplay(false);

  for (int r = 4; r <= 44; r += 4) {
    display.clearDisplay();
    display.drawCircle(64, 32, r, SSD1306_WHITE);
    if (r > 12) display.drawCircle(64, 32, r - 10, SSD1306_WHITE);
    if (r > 8) drawBolt();
    display.display();
  }
  delay(250);

  const char* brand = "TechTadka";
  char buf[12];
  for (int i = 1; i <= 9; i++) {
    strncpy(buf, brand, i);
    buf[i] = 0;
    display.clearDisplay();
    display.setFont();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 2);
    display.print(buf);
    display.display();
    delay(40);
  }

  for (int x = -46; x <= 10; x += 8) {
    drawBrandScene(x > 10 ? 10 : x, 40);
    display.display();
  }
  for (int off = 40; off >= 0; off -= 8) {
    drawBrandScene(10, off);
    display.display();
  }

  const char* steps[4] = { "Starting ADC...", "Checking OLED...", "Probe inputs ready", "All systems go!" };
  for (int s = 0; s < 4; s++) {
    for (int p = 0; p <= 25; p += 5) {
      int pct = s * 25 + p;
      drawBrandScene(10, 0);
      drawSmall(steps[s], 4, 46);
      display.drawRect(4, 57, 120, 6, SSD1306_WHITE);
      display.fillRect(6, 59, (116 * pct) / 100, 2, SSD1306_WHITE);
      display.display();
      delay(20);
    }
  }
  delay(200);
  wipe();
}

OhmResult measureOhms() {
  OhmResult r = { OHM_OPEN, 0 };
  refsOff();
  pinMode(PIN_REF10K, OUTPUT);
  digitalWrite(PIN_REF10K, HIGH);
  delay(3);
  float v = readMv(PIN_NODE, 32);
  if (v >= 3000.0f) { refsOff(); return r; }
  if (v >= 150.0f) {
    r.state = OHM_VALUE;
    r.ohms  = R_HIGH * v / (VCC_MV - v);
    refsOff();
    return r;
  }
  pinMode(PIN_REF10K, INPUT);
  pinMode(PIN_REF1K, OUTPUT);
  digitalWrite(PIN_REF1K, HIGH);
  delay(3);
  float v1 = readMv(PIN_NODE, 32);
  refsOff();
  if (v1 < SHORT_MV) { r.state = OHM_SHORT; return r; }
  r.state = OHM_VALUE;
  r.ohms  = R_LOW * v1 / (VCC_MV - v1);
  return r;
}

void screenVolt() {
  float pinMv = readMv(PIN_VIN, 64);
  float pv = pinMv - PIN_OFFSET_MV;
  if (pv < 0) pv = 0;
  float v = pv / 1000.0f * ((R_TOP + R_BOTTOM) / R_BOTTOM) * V_CAL;

  if (vFilt < 0) vFilt = v;
  else {
    float d = fabsf(v - vFilt);
    float a = (d > 0.5f) ? 0.6f : ((d > 0.15f) ? 0.3f : 0.1f);
    vFilt += a * (v - vFilt);
  }

  static bool zeroed = true;
  if (zeroed && vFilt > V_DEADBAND + 0.10f)  zeroed = false;
  else if (!zeroed && vFilt < V_DEADBAND)    zeroed = true;
  float shown = zeroed ? 0.0f : vFilt;
  if (shown == 0.0f || fabsf(shown - vShown) >= 0.025f) vShown = shown;

#if DEBUG_SERIAL
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 500) {
    lastPrint = millis();
    Serial.printf("GPIO34 = %.0f mV   ->   %.2f V\n", pinMv, vShown);
  }
#endif

  drawHeader("DC VOLTAGE");
  if (pinMv > 3000.0f) {
    drawValue("OVER", "", 28);
    drawSegBarAnim(1.0f, 46);
    footerText("V JACK", "TOO HIGH!");
  } else {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.2f", vShown);
    drawValue(buf, U_V, 28);
    bool low = (vShown <= 5.0f);
    drawSegBarAnim(vShown / (low ? 5.0f : 16.0f), 46);
    footerText("V JACK", low ? "0-5V" : "0-16V");
  }
}

void screenOhm() {
  OhmResult r = measureOhms();
  bool cont = (r.state == OHM_SHORT) || (r.state == OHM_VALUE && r.ohms < CONT_OHMS);
  setBeep(cont);

  if (r.state == OHM_VALUE) {
    if (rShown <= 0 || fabsf(r.ohms - rShown) > 0.03f * rShown) rShown = r.ohms;
  } else {
    rShown = 0;
  }

  drawHeader("CONTINUITY");
  char buf[16];
  if (r.state == OHM_OPEN) {
    drawValue("OPEN", "", 28);
    drawSegBarAnim(0.0f, 46);
  } else if (r.state == OHM_SHORT) {
    drawValue("<30", U_OHM, 28);
    drawSegBarAnim(0.0f, 46);
  } else {
    const char* unit = U_OHM;
    if (rShown < 1000.0f)         snprintf(buf, sizeof(buf), "%.0f", rShown);
    else if (rShown < 10000.0f)   { snprintf(buf, sizeof(buf), "%.2f", rShown / 1000.0f); unit = U_KOHM; }
    else if (rShown < 1000000.0f) { snprintf(buf, sizeof(buf), "%.1f", rShown / 1000.0f); unit = U_KOHM; }
    else                          { snprintf(buf, sizeof(buf), "%.2f", rShown / 1000000.0f); unit = U_MOHM; }
    drawValue(buf, unit, 28);
    drawSegBarAnim((log10f(max(rShown, 10.0f)) - 1.0f) / 4.0f, 46);
  }

  if (cont) {
    if ((millis() / 250) % 2 == 0) {
      display.fillRect(0, 54, SCREEN_W, 10, SSD1306_WHITE);
      display.setFont();
      display.setTextSize(1);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(4, 55);
      display.print("CONTINUITY  *BEEP*");
    } else {
      footerText("OHM JACK", "CONNECTED");
    }
  } else if (r.state == OHM_OPEN) {
    footerText("OHM JACK", "NO CONT.");
  } else {
    footerText("OHM JACK", "RESISTOR");
  }
}

void screenLed() {
  float v = readMv(PIN_NODE, 32);
  lFilt = (lFilt < 0) ? v : lFilt + 0.4f * (v - lFilt);

  drawHeader("LED / DIODE");

  const char* status;
  const char* l2;
  char l1[20];
  bool good = false;
  l1[0] = 0;

  if (lFilt >= 3000.0f) {
    status = "OPEN";
    snprintf(l1, sizeof(l1), "Open/reversed");
    l2 = "Flip & retry";
  } else if (lFilt < SHORT_MV) {
    status = "SHORT";
    snprintf(l1, sizeof(l1), "Short circuit");
    l2 = "Check probes";
  } else if (lFilt < 1000.0f) {
    status = "DIODE";
    snprintf(l1, sizeof(l1), "Vf %.2f V", lFilt / 1000.0f);
    l2 = (lFilt < 450.0f) ? "Schottky/Ge" : "Silicon diode";
  } else {
    good = true;
    status = "GOOD";
    snprintf(l1, sizeof(l1), "Vf %.2f V", lFilt / 1000.0f);
    if (lFilt < 1700.0f)      l2 = "IR / Deep red";
    else if (lFilt < 2300.0f) l2 = "Red/Yel/Grn";
    else                      l2 = "Blue / White";
  }

  drawLedIcon(good, (millis() / 300) % 2 == 0);
  drawFitLeft(status, 46, 26, 78);
  drawSmall(l1, 46, 38);
  drawSmall(l2, 46, 47);
  footerText("OHM JACK", "RED=+LEG");
}

void gotoMode(int m) {
  setBeep(false);
  refsOff();
  wipe();
  mode  = m;
  vFilt = -1; vShown = 0; rShown = 0; lFilt = -1; barAnim = 0;
  if (mode == MODE_LED) {
    pinMode(PIN_REF1K, OUTPUT);
    digitalWrite(PIN_REF1K, HIGH);
    delay(5);
  }
  lastDraw = 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  setBeep(false);
  refsOff();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found - check SDA/SCL wires");
    while (true) delay(1000);
  }
  display.cp437(true);
  display.setTextWrap(false);
  display.clearDisplay();
  display.display();

  bootAnimation();
  mode = MODE_VOLT;
}

void loop() {
  if (buttonPressed()) gotoMode((mode + 1) % MODE_COUNT);

  if (millis() - lastDraw >= 100) {
    lastDraw = millis();
    display.clearDisplay();
    switch (mode) {
      case MODE_VOLT: screenVolt(); break;
      case MODE_OHM:  screenOhm();  break;
      case MODE_LED:  screenLed();  break;
    }
    display.display();
  }
}
