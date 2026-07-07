#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'<','0','=','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {26, 25, 33, 32};
Keypad kpad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define LED_RED    2
#define LED_GREEN  4
#define LED_BLUE   5
#define BUZZER     18

enum Role { ROLE_NONE = 0, ROLE_ADMIN, ROLE_OPERATOR, ROLE_VIEWER };

struct User {
  char id[5];
  Role role;
  char name[17];      // shown on LCD row 0
  char welcome[17];   // shown on LCD row 1 on check-in
  char goodbye[17];   // shown on LCD row 1 on check-out
  bool checkedIn;     // toggles each time this ID logs a successful entry
};

#define MAX_USERS 10   // 4 built-in + up to 6 enrolled at runtime

User USERS[MAX_USERS] = {
  {"1234", ROLE_ADMIN,    "Alice (Admin)", "Welcome, Boss! ", "Bye, Boss!     ", false},
  {"5678", ROLE_OPERATOR, "Bob (Operator)", "Welcome, Bob!  ", "Bye, Bob!      ", false},
  {"9012", ROLE_OPERATOR, "Carol (Oper.) ", "Welcome, Carol!", "Bye, Carol!    ", false},
  {"3456", ROLE_VIEWER,   "Dave (Viewer) ", "Welcome, Dave! ", "Bye, Dave!     ", false},
};
int USER_COUNT = 4;   // grows as members are enrolled, capped at MAX_USERS

enum State {
  ST_IDLE, ST_INPUT, ST_GRANTED, ST_DENIED, ST_LOCKOUT, ST_INCOMPLETE,
  ST_ENROLL_AUTH, ST_ENROLL_NEWID, ST_ENROLL_ROLE, ST_ENROLL_RESULT
};
State state = ST_IDLE;

char          inputBuf[5] = {0};
int           inputLen    = 0;
int           failCount   = 0;
unsigned long lastKeyTime = 0;
unsigned long stateTimer  = 0;

#define TIMEOUT_MS       7000UL
#define LOCKOUT_MS      15000UL
#define FEEDBACK_MS      3000UL
#define INCOMPLETE_MS     800UL
#define ENROLL_TIMEOUT_MS 30000UL   // enrollment is multi-step, so it gets more time than a normal login

// ---- Member enrollment (Admin-only, triggered by the 'D' key from idle) ----
char pendingNewId[5] = {0};   // ID typed in ST_ENROLL_NEWID, used once role is picked
char enrollMsg1[17]  = {0};   // result screen line 1
char enrollMsg2[17]  = {0};   // result screen line 2

// Shared renderer for any "type up to 4 digits, see them masked" screen —
// used by the normal login AND both enrollment prompts, so the masking
// logic only lives in one place.
void showMaskedInput(const char* title, const char* label) {
  char stars[5] = "____";
  for (int i = 0; i < inputLen && i < 4; i++) stars[i] = '*';
  stars[4] = 0;
  char line[17];
  snprintf(line, 17, "%-12s%s", label, stars);
  lcd.clear();
  lcdRow(0, title);
  lcdRow(1, line);
}

// =====================================================================
// PHASE 2 additions start here
// =====================================================================

// ---- network config ----
const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_PASS  = "";
const char* SERVER_URL = "https://fluffy-pandas-repair.loca.lt/post";

const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 19800;   // IST = UTC+5:30, change if needed
const int   DST_OFFSET_SEC = 0;

bool wifiConnected = false;
bool timeSynced    = false;

unsigned long lastWifiPoll       = 0;
const unsigned long WIFI_POLL_MS = 2000;

unsigned long lastFlushAttempt      = 0;
const unsigned long FLUSH_RETRY_MS  = 5000;

// ---- offline queue ----
const int QUEUE_MAX = 10;
String offlineQueue[QUEUE_MAX];
int    queueCount = 0;

// =====================================================================

void allLedsOff() {
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_BLUE,  LOW);
  digitalWrite(LED_RED,   LOW);
}

// =====================================================================
// Non-blocking effect scheduler
// Each pattern (blink + tone sequence) is a table of steps. updateEffects()
// runs every loop() tick and advances one step once "hold" ms have passed,
// replacing every delay()-based blink/tone sequence from Phase 1.
// =====================================================================

struct EffectStep {
  int ledOnPin;    // pin to drive HIGH this step, -1 = none
  int ledOffPin;   // pin to drive LOW this step,  -1 = none
  int toneFreq;    // buzzer frequency, 0 = no tone this step
  int toneDur;     // tone's own duration (tone() itself is non-blocking)
  unsigned long hold; // ms to wait before moving to the next step
};

// Explicit prototypes: Arduino auto-generates function prototypes and
// inserts them near the top of the file, before it has seen this struct
// definition, which breaks with "'EffectStep' does not name a type".
// Declaring these here ourselves makes the auto-generator skip them.
void applyEffectStep(const EffectStep& s);
void startEffect(const EffectStep* steps, int len);

const EffectStep EFFECT_ADMIN[] = {
  { LED_GREEN, -1,          0,   0,  120 },
  { -1,        LED_GREEN,   0,   0,   80 },
  { LED_GREEN, -1,          0,   0,  120 },
  { -1,        LED_GREEN,   0,   0,   80 },
  { LED_GREEN, -1,          0,   0,  120 },
  { -1,        LED_GREEN,   0,   0,   80 },
  { LED_GREEN, -1,       1000,  80,  100 },
  { -1,        -1,       1500,  80,  100 },
  { -1,        -1,       2000, 200,  200 },
};

const EffectStep EFFECT_OPERATOR_BOB[] = {
  { LED_BLUE, -1,          0,   0,  250 },
  { -1,       LED_BLUE,    0,   0,  150 },
  { LED_BLUE, -1,          0,   0,  250 },
  { -1,       LED_BLUE,    0,   0,  150 },
  { LED_BLUE, -1,       1200, 150,  200 },
  { -1,       -1,       1200, 150,  150 },
};

const EffectStep EFFECT_OPERATOR_CAROL[] = {
  { LED_BLUE,  -1,          0,   0,  200 },
  { LED_GREEN, LED_BLUE,    0,   0,  200 },
  { LED_BLUE,  LED_GREEN,   0,   0,  200 },
  { LED_GREEN, LED_BLUE,    0,   0,  200 },
  { LED_BLUE,  LED_GREEN, 1100, 100,  150 },
  { -1,        -1,        1400, 150,  150 },
};

const EffectStep EFFECT_VIEWER[] = {
  { LED_RED,  -1,          0,   0,  400 },
  { -1,       LED_RED,     0,   0,  100 },
  { LED_BLUE, -1,        900, 400,  400 },
};

const EffectStep EFFECT_LOCKOUT[] = {
  { LED_RED, -1,          0,   0,  200 },
  { -1,      LED_RED,     0,   0,  200 },
  { LED_RED, -1,          0,   0,  200 },
  { -1,      LED_RED,     0,   0,  200 },
  { LED_RED, -1,          0,   0,  200 },
  { -1,      LED_RED,     0,   0,  200 },
  { LED_RED, -1,        200,1000, 1000 },
};

// Same for every role - a check-out doesn't need role-specific styling,
// just needs to sound/look clearly different from a check-in.
const EffectStep EFFECT_CHECKOUT[] = {
  { LED_BLUE, -1,          0,   0,  150 },
  { -1,       LED_BLUE,    0,   0,  100 },
  { LED_BLUE, -1,        600, 150,  150 },
  { -1,       LED_BLUE,    0,   0,   50 },
};

const EffectStep* activeEffect    = nullptr;
int   activeEffectLen             = 0;
int   effectIndex                 = 0;
unsigned long effectStepStart     = 0;
bool  effectRunning               = false;

void applyEffectStep(const EffectStep& s) {
  if (s.ledOnPin  >= 0) digitalWrite(s.ledOnPin,  HIGH);
  if (s.ledOffPin >= 0) digitalWrite(s.ledOffPin, LOW);
  if (s.toneFreq  >  0) tone(BUZZER, s.toneFreq, s.toneDur); // non-blocking
}

void startEffect(const EffectStep* steps, int len) {
  activeEffect    = steps;
  activeEffectLen = len;
  effectIndex     = 0;
  effectStepStart = millis();
  effectRunning   = true;
  applyEffectStep(steps[0]);
}

// Call every loop() tick. Advances the active effect's steps over time
// instead of blocking with delay().
void updateEffects() {
  if (!effectRunning) return;
  if (millis() - effectStepStart >= activeEffect[effectIndex].hold) {
    effectIndex++;
    if (effectIndex >= activeEffectLen) {
      effectRunning = false;
      return;
    }
    effectStepStart = millis();
    applyEffectStep(activeEffect[effectIndex]);
  }
}

void successSignal(const User* u) {
  allLedsOff();
  switch (u->role) {
    case ROLE_ADMIN:
      startEffect(EFFECT_ADMIN, sizeof(EFFECT_ADMIN) / sizeof(EFFECT_ADMIN[0]));
      break;
    case ROLE_OPERATOR:
      if (strcmp(u->id, "5678") == 0)
        startEffect(EFFECT_OPERATOR_BOB, sizeof(EFFECT_OPERATOR_BOB) / sizeof(EFFECT_OPERATOR_BOB[0]));
      else
        startEffect(EFFECT_OPERATOR_CAROL, sizeof(EFFECT_OPERATOR_CAROL) / sizeof(EFFECT_OPERATOR_CAROL[0]));
      break;
    default:
      startEffect(EFFECT_VIEWER, sizeof(EFFECT_VIEWER) / sizeof(EFFECT_VIEWER[0]));
      break;
  }
}

// Already non-blocking in the original: LED set once, tone() auto-stops
// itself via hardware timer without a delay() call. Left as-is.
void deniedSignal() {
  digitalWrite(LED_RED, HIGH);
  tone(BUZZER, 300, 600);
}

void lockoutSignal() {
  startEffect(EFFECT_LOCKOUT, sizeof(EFFECT_LOCKOUT) / sizeof(EFFECT_LOCKOUT[0]));
}

void checkoutSignal() {
  allLedsOff();
  startEffect(EFFECT_CHECKOUT, sizeof(EFFECT_CHECKOUT) / sizeof(EFFECT_CHECKOUT[0]));
}

void lcdRow(int r, const char* msg) {
  lcd.setCursor(0, r);
  lcd.print(msg);
}

void showIdle() {
  lcd.clear();
  lcdRow(0, " SECURE TERMINAL");
  lcdRow(1, "  Enter ID: ____");
}

void showInputMasked() {
  char mask[17] = "  Enter ID: ____";
  for (int i = 0; i < inputLen && i < 4; i++) mask[12 + i] = '*';
  lcd.clear();
  lcdRow(0, " SECURE TERMINAL");
  lcdRow(1, mask);
}

void showGranted(const User* u, bool isCheckIn) {
  lcd.clear();
  lcdRow(0, u->name);
  lcdRow(1, isCheckIn ? u->welcome : u->goodbye);
}

void showDenied() {
  lcd.clear();
  lcdRow(0, "  ACCESS DENIED ");
  char buf[17];
  snprintf(buf, 17, " Attempt %d of 3 ", failCount);
  lcdRow(1, buf);
}

void showLockout(unsigned long remaining) {
  lcd.clear();
  lcdRow(0, "*** LOCKED OUT **");
  char buf[17];
  snprintf(buf, 17, "  Wait %2lus ...  ", remaining / 1000 + 1);
  lcdRow(1, buf);
}

User* findUser(const char* id) {
  for (int i = 0; i < USER_COUNT; i++)
    if (strcmp(USERS[i].id, id) == 0) return &USERS[i];
  return nullptr;
}

// ---- Enrollment screens ----
void showEnrollAuthScreen()  { showMaskedInput(" ADMIN VERIFY   ", "PIN:"); }
void showEnrollNewIdScreen() { showMaskedInput(" ENROLL NEW ID  ", "New ID:"); }

void showEnrollRoleScreen() {
  lcd.clear();
  lcdRow(0, "Select Role:");
  lcdRow(1, "1=Adm 2=Op 3=Vw");
}

void showEnrollResultScreen() {
  lcd.clear();
  lcdRow(0, enrollMsg1);
  lcdRow(1, enrollMsg2);
}

// Sets the two result-screen lines and switches state — used for every
// enrollment outcome (success, admin-only, duplicate ID, storage full).
void setEnrollResult(const char* line1, const char* line2) {
  strncpy(enrollMsg1, line1, 16); enrollMsg1[16] = 0;
  strncpy(enrollMsg2, line2, 16); enrollMsg2[16] = 0;
  showEnrollResultScreen();
  state      = ST_ENROLL_RESULT;
  stateTimer = millis();
}

// Actually adds a new member. Returns false if storage is full or the ID
// is already taken (caller checks for duplicates before calling this too,
// but this guard keeps addUser() safe to call on its own).
bool addUser(const char* id, Role role) {
  if (USER_COUNT >= MAX_USERS) return false;
  if (findUser(id)) return false;

  User* u = &USERS[USER_COUNT];
  strncpy(u->id, id, 4); u->id[4] = 0;
  u->role = role;
  u->checkedIn = false;

  const char* roleName = (role == ROLE_ADMIN) ? "Admin" :
                          (role == ROLE_OPERATOR) ? "Operator" : "Viewer";
  snprintf(u->name, 17, "New %-8s", roleName);
  snprintf(u->welcome, 17, "Welcome aboard!");
  snprintf(u->goodbye, 17, "Goodbye!        ");

  USER_COUNT++;
  Serial.printf("[ENROLL] Added id=%s role=%d (count=%d)\n", u->id, role, USER_COUNT);
  return true;
}

void resetInput() {
  memset(inputBuf, 0, sizeof(inputBuf));
  inputLen    = 0;
  lastKeyTime = 0;
}

// =====================================================================
// PHASE 2: network state machine (all non-blocking)
// =====================================================================

// Polls WiFi.status() on a timer instead of blocking-waiting for connect.
void updateWifiState() {
  unsigned long now = millis();
  if (now - lastWifiPoll < WIFI_POLL_MS) return;
  lastWifiPoll = now;

  bool nowConnected = (WiFi.status() == WL_CONNECTED);

  if (nowConnected && !wifiConnected) {
    wifiConnected = true;
    Serial.print("[WIFI] Connected, IP: ");
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER); // kick off NTP, non-blocking
  } else if (!nowConnected && wifiConnected) {
    wifiConnected = false;
    timeSynced = false;
    Serial.println("[WIFI] Connection lost.");
  }
}

// time(nullptr) returns seconds-since-epoch; before NTP succeeds it's a
// small number close to 0, so checking against a real-world epoch tells
// us sync succeeded WITHOUT ever blocking to wait for it.
void checkTimeSync() {
  if (!wifiConnected || timeSynced) return;
  time_t now = time(nullptr);
  if (now > 1700000000UL) { // sometime after Nov 2023 -> NTP has landed
    timeSynced = true;
    Serial.println("[NTP] Time synced.");
  }
}

String getTimestamp() {
  if (timeSynced) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);
    return String(buf);
  }
  return "uptime_" + String(millis()); // fallback marker if not synced yet
}

// =====================================================================
// PHASE 2: JSON payload
// =====================================================================

String buildPayload(const User* u, const char* result) {
  StaticJsonDocument<256> doc;   // fixed-size, stack-allocated -> no heap fragmentation
  doc["timestamp"] = getTimestamp();
  doc["result"]    = result;
  if (u) {
    doc["id"]   = u->id;
    doc["name"] = u->name;
    doc["role"] = (int)u->role;
  } else {
    doc["id"]   = inputBuf;
    doc["name"] = "unknown";
    doc["role"] = (int)ROLE_NONE;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String buildEnrollPayload(const char* newId, Role role) {
  StaticJsonDocument<256> doc;
  doc["timestamp"] = getTimestamp();
  doc["result"]    = "ENROLL";
  doc["new_id"]    = newId;
  doc["role"]      = (int)role;
  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================================
// PHASE 2: offline queue + HTTP
// =====================================================================

void enqueue(const String& payload) {
  if (queueCount >= QUEUE_MAX) {
    // Policy: drop oldest to make room for newest (documented in report)
    for (int i = 1; i < QUEUE_MAX; i++) offlineQueue[i - 1] = offlineQueue[i];
    offlineQueue[QUEUE_MAX - 1] = payload;
    Serial.println("[QUEUE] Full - dropped oldest entry.");
  } else {
    offlineQueue[queueCount++] = payload;
    Serial.printf("[QUEUE] Stored (%d/%d)\n", queueCount, QUEUE_MAX);
  }
}

// Declared globally, not as a local variable inside postToServer(). The
// TLS context this object carries is large enough that creating a fresh
// one on the stack every call risks overflowing the loop task's stack -
// which shows up as an unlabeled "assert failed" crash exactly like the
// one this fixes. One reused object avoids that entirely.
WiFiClientSecure secureClient;

bool postToServer(const String& payload) {
  if (!wifiConnected) return false;

  HTTPClient http;
  bool began;

  // https:// URLs need an explicit secure client on this HTTPClient version,
  // otherwise begin(url) alone can crash deep in the TLS stack instead of
  // just failing. setInsecure() skips certificate validation - fine for a
  // demo/hackathon server, not something to ship to production as-is.
  if (String(SERVER_URL).startsWith("https")) {
    secureClient.setInsecure();
    began = http.begin(secureClient, SERVER_URL);
    if (!began) { Serial.println("[HTTP] begin() failed (https)"); return false; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("bypass-tunnel-reminder", "true"); // needed for localtunnel.me URLs
    http.setTimeout(3000);
    int code = http.POST(payload);
    http.end();
    secureClient.stop(); // release the TLS session before the next call reuses this object
    Serial.printf("[HTTP] POST -> %d\n", code);
    return (code >= 200 && code < 300);
  }

  began = http.begin(SERVER_URL);
  if (!began) { Serial.println("[HTTP] begin() failed"); return false; }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int code = http.POST(payload);
  http.end();
  Serial.printf("[HTTP] POST -> %d\n", code);
  return (code >= 200 && code < 300);
}

void sendOrQueue(const String& payload) {
  if (wifiConnected && timeSynced) {
    if (!postToServer(payload)) enqueue(payload);
  } else {
    enqueue(payload);
  }
}

void flushQueue() {
  if (!wifiConnected || queueCount == 0) return;
  Serial.printf("[QUEUE] Flushing %d entries...\n", queueCount);
  int sent = 0;
  while (sent < queueCount) {
    if (postToServer(offlineQueue[sent])) sent++;
    else break; // stop at first failure, keep the rest queued for next retry
  }
  if (sent > 0) {
    for (int i = sent; i < queueCount; i++) offlineQueue[i - sent] = offlineQueue[i];
    queueCount -= sent;
    Serial.printf("[QUEUE] Flushed %d, %d remaining.\n", sent, queueCount);
  }
}

void updateFlush() {
  unsigned long now = millis();
  if (!wifiConnected || queueCount == 0) return;
  if (now - lastFlushAttempt < FLUSH_RETRY_MS) return;
  lastFlushAttempt = now;
  flushQueue();
}

// =====================================================================

void setup() {
  Serial.begin(115200);
  pinMode(LED_RED,   OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE,  OUTPUT);
  pinMode(BUZZER,    OUTPUT);
  allLedsOff();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("[WIFI] Connecting (non-blocking)...");

  showIdle();
  state = ST_IDLE;
  Serial.println("[BOOT] Terminal ready.");
}

void loop() {
  unsigned long now = millis();

  // --- background tasks: run every tick, never block ---
  updateWifiState();
  checkTimeSync();
  updateFlush();
  updateEffects();

  if (state == ST_INCOMPLETE) {
    if (now - stateTimer >= INCOMPLETE_MS) {
      if (inputLen > 0) { state = ST_INPUT; showInputMasked(); }
      else              { state = ST_IDLE;  showIdle(); }
    }
    return;
  }

  if (state == ST_LOCKOUT) {
    unsigned long elapsed = now - stateTimer;
    if (elapsed >= LOCKOUT_MS) {
      allLedsOff();
      failCount = 0;
      resetInput();
      state = ST_IDLE;
      showIdle();
    } else {
      static unsigned long lastLockUpdate = 0;
      if (now - lastLockUpdate > 500) {
        lastLockUpdate = now;
        showLockout(LOCKOUT_MS - elapsed);
      }
    }
    return;
  }

  // GRANTED / DENIED / ENROLL_RESULT all just show a message for FEEDBACK_MS
  // then return to idle — same timer pattern, so they share one block.
  if (state == ST_GRANTED || state == ST_DENIED || state == ST_ENROLL_RESULT) {
    if (now - stateTimer >= FEEDBACK_MS) {
      allLedsOff();
      noTone(BUZZER);
      resetInput();
      state = ST_IDLE;
      showIdle();
    }
    return;
  }

  // Typing timeout: normal login gets the short timeout, enrollment
  // (a multi-step flow) gets the longer one.
  if (state == ST_INPUT && inputLen > 0) {
    if (now - lastKeyTime >= TIMEOUT_MS) {
      Serial.println("[TIMEOUT] Input cleared.");
      resetInput();
      state = ST_IDLE;
      showIdle();
      return;
    }
  }
  if ((state == ST_ENROLL_AUTH || state == ST_ENROLL_NEWID)
      && now - lastKeyTime >= ENROLL_TIMEOUT_MS) {
    Serial.println("[TIMEOUT] Enrollment cancelled.");
    resetInput();
    state = ST_IDLE;
    showIdle();
    return;
  }
  // Role-selection screen has no typed digits, so it's timed off lastKeyTime directly.
  if (state == ST_ENROLL_ROLE && now - lastKeyTime >= ENROLL_TIMEOUT_MS) {
    Serial.println("[TIMEOUT] Enrollment cancelled.");
    resetInput();
    state = ST_IDLE;
    showIdle();
    return;
  }

  char key = kpad.getKey();
  if (!key) return;

  tone(BUZZER, 1200, 40);
  lastKeyTime = now;
  Serial.printf("[KEY] '%c'\n", key);

  // 'D' from idle starts admin-only member enrollment.
  if (state == ST_IDLE && key == 'D') {
    resetInput();
    lastKeyTime = now;
    state = ST_ENROLL_AUTH;
    showEnrollAuthScreen();
    return;
  }

  // Role-selection is a single keypress, not accumulated digits.
  if (state == ST_ENROLL_ROLE) {
    Role chosen = ROLE_NONE;
    if      (key == '1') chosen = ROLE_ADMIN;
    else if (key == '2') chosen = ROLE_OPERATOR;
    else if (key == '3') chosen = ROLE_VIEWER;
    else if (key == 'D') { resetInput(); state = ST_IDLE; showIdle(); return; } // cancel
    else return; // ignore anything else, stay on this screen

    if (addUser(pendingNewId, chosen)) {
      char line2[17];
      snprintf(line2, 17, "ID: %-4s        ", pendingNewId);
      setEnrollResult(" MEMBER ADDED   ", line2);
      sendOrQueue(buildEnrollPayload(pendingNewId, chosen));
    } else {
      setEnrollResult(" ENROLL FAILED  ", " Storage full   ");
    }
    resetInput();
    return;
  }

  if (key == '<') {
    if (inputLen > 0) {
      inputLen--;
      inputBuf[inputLen] = 0;
      if (state == ST_ENROLL_AUTH)        { showEnrollAuthScreen(); }
      else if (state == ST_ENROLL_NEWID)  { showEnrollNewIdScreen(); }
      else if (inputLen == 0)             { state = ST_IDLE;  showIdle(); }
      else                                 { state = ST_INPUT; showInputMasked(); }
    } else if (state == ST_ENROLL_AUTH || state == ST_ENROLL_NEWID) {
      state = ST_IDLE; showIdle(); // backspace on empty enrollment field cancels it
    }
    return;
  }

  if (key == '=') {
    if (state == ST_ENROLL_AUTH) {
      if (inputLen < 4) return; // ignore submit until 4 digits typed
      inputBuf[4] = 0;
      const User* u = findUser(inputBuf);
      resetInput();
      if (u && u->role == ROLE_ADMIN) {
        state = ST_ENROLL_NEWID;
        showEnrollNewIdScreen();
      } else {
        setEnrollResult(" ADMIN ONLY     ", " Access denied  ");
      }
      return;
    }

    if (state == ST_ENROLL_NEWID) {
      if (inputLen < 4) return;
      inputBuf[4] = 0;
      if (findUser(inputBuf)) {
        setEnrollResult(" ID ALREADY USED", " Try another ID ");
        resetInput();
      } else {
        strncpy(pendingNewId, inputBuf, 4); pendingNewId[4] = 0;
        resetInput();
        state = ST_ENROLL_ROLE;
        showEnrollRoleScreen();
      }
      return;
    }

    // --- normal login submit (ST_IDLE / ST_INPUT) ---
    if (inputLen < 4) {
      lcd.clear();
      lcdRow(0, " INCOMPLETE ID  ");
      lcdRow(1, " Need 4 digits  ");
      state      = ST_INCOMPLETE;
      stateTimer = millis();
      return;
    }
    inputBuf[4] = 0;
    User* u = findUser(inputBuf);
    if (u) {
      failCount = 0;
      // Toggle check-in/check-out: second successful entry for the same
      // ID logs them out instead of granting access again.
      bool wasCheckedIn = u->checkedIn;
      u->checkedIn = !wasCheckedIn;

      if (!wasCheckedIn) {
        successSignal(u);
        showGranted(u, true);
        Serial.printf("[CHECK-IN] %s\n", u->name);
        sendOrQueue(buildPayload(u, "CHECK_IN"));
      } else {
        checkoutSignal();
        showGranted(u, false);
        Serial.printf("[CHECK-OUT] %s\n", u->name);
        sendOrQueue(buildPayload(u, "CHECK_OUT"));
      }
      state      = ST_GRANTED;
      stateTimer = millis();
    } else {
      failCount++;
      Serial.printf("[DENIED] fail=%d\n", failCount);

      // Phase 2: log the failed access attempt
      sendOrQueue(buildPayload(nullptr, "DENIED"));

      if (failCount >= 3) {
        lockoutSignal();
        state      = ST_LOCKOUT;
        stateTimer = millis();
        showLockout(LOCKOUT_MS);

        // Phase 2: log the lockout event itself
        sendOrQueue(buildPayload(nullptr, "LOCKOUT"));
      } else {
        deniedSignal();
        showDenied();
        state      = ST_DENIED;
        stateTimer = millis();
      }
    }
    return;
  }

  if (inputLen < 4 && key >= '0' && key <= '9') {
    inputBuf[inputLen++] = key;
    inputBuf[inputLen]   = 0;
    if (state == ST_ENROLL_AUTH)       { showEnrollAuthScreen(); }
    else if (state == ST_ENROLL_NEWID) { showEnrollNewIdScreen(); }
    else { state = ST_INPUT; showInputMasked(); }
  }
}
