# Electrothon — Secure Access Terminal (ESP32)

A two-phase embedded access-control terminal built for the Electrothon hackathon.

- **Phase 1:** Local keypad + LCD + LED/buzzer access control, fully non-blocking (no `delay()` anywhere).
- **Phase 2:** WiFi + NTP time sync, JSON logging to a remote server, offline queue with auto-recovery, admin-only member enrollment, and check-in/check-out tracking.

---

## Live Wokwi Project

**Simulation link:** [https://wokwi.com/projects/466788091244715009](https://wokwi.com/projects/466788091244715009)

---

## Project structure

```
Entry-Terminal/
├── Server/
│   ├── node_modules/
│   ├── package-lock.json
│   ├── package.json
│   └── server.js
├── Wokwi files/
│   ├── diagram.json
│   ├── libraries.txt
│   ├── sketch.ino
│   └── wokwi-project.txt
├── .gitignore
├── Report.pdf
└── README.md
```

---

## 1. Hardware / Wokwi circuit

Open [wokwi.com](https://wokwi.com) → New Project → **ESP32**. Wire these parts to the pins used in the sketch:

| Component    | Pins                                            |
| ------------ | ----------------------------------------------- |
| 4x4 Keypad   | Rows: `13, 12, 14, 27` — Cols: `26, 25, 33, 32` |
| I2C LCD 16x2 | SDA `21`, SCL `22` (address `0x27`)             |
| Red LED      | `2`                                             |
| Green LED    | `4`                                             |
| Blue LED     | `5`                                             |
| Buzzer       | `18`                                            |

Add a resistor (~220Ω) in series with each LED.

### Libraries

Add a `libraries.txt` file in the Wokwi project with:

```
LiquidCrystal_I2C
Keypad
ArduinoJson
```

(`WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`, and `time.h` are built into the ESP32 core — no install needed.)

Paste `electrothon_phase2.ino` into `sketch.ino`.

---

## 2. Run the server

The server receives every access event from the ESP32 (check-in, check-out, denied, lockout, enrollment) and stores it in a SQLite database, with a live dashboard to view who's checked in and when.

```bash
cd server
npm install
npm start
```

You should see:

```
Server listening on port 3000
```

View it locally at:

- **`http://localhost:3000/dashboard`** — human-readable dashboard (who's checked in, session history, full activity feed)
- **`http://localhost:3000/logs`** — raw JSON of the last 200 records

---

## 3. Give the ESP32 a public URL to talk to

Wokwi's simulated ESP32 runs in the cloud — it **cannot** reach `localhost` on your own machine. You need a public URL pointing at your local server. Two ways to do this:

### Option A — Temporary tunnel (fast, good for testing/demos)

In a separate terminal, with the server still running:

```bash
npx localtunnel --port 3000
```

This prints a temporary public URL, for example:

```
your url is: https://fluffy-pandas-repair.loca.lt
```

**Keep this terminal window open** the whole time you're testing — the moment you close it, the URL stops working.

In `electrothon_phase2.ino`, set:

```cpp
const char* SERVER_URL = "https://fluffy-pandas-repair.loca.lt/post";
```

> Replace the subdomain with whatever URL your own `localtunnel` run prints — it's different every time you start a new tunnel session unless you reserve a fixed one.

View the live dashboard at the same base URL:

```
https://fluffy-pandas-repair.loca.lt/dashboard
```

### Option B — Permanent deployment (recommended for final submission)

1. Push the `server/` folder to its own GitHub repo.
2. Deploy on **Railway** or **Render** (free tier): New Web Service → connect the repo → build command `npm install` → start command `npm start`.
3. Copy the public URL it gives you (e.g. `https://your-app.up.railway.app`).
4. Update `SERVER_URL` in the `.ino` to `https://your-app.up.railway.app/post`.

This URL stays live permanently (subject to the host's free-tier sleep/wake behavior) and doesn't depend on your laptop staying on.

---

## 4. Run the simulation

1. In Wokwi, press the green **Play** button.
2. Open the **Serial Monitor** (115200 baud) to watch `[WIFI]`, `[NTP]`, `[QUEUE]`, `[HTTP]` logs.
3. The ESP32 connects automatically to `Wokwi-GUEST` (Wokwi's built-in virtual WiFi, no password needed).

---

## 5. Using the terminal

**Built-in members:**

| ID     | Role             |
| ------ | ---------------- |
| `1234` | Admin (Alice)    |
| `5678` | Operator (Bob)   |
| `9012` | Operator (Carol) |
| `3456` | Viewer (Dave)    |

**Check in / check out:** enter a 4-digit ID + `=`. First time → check-in (welcome message). Enter the _same_ ID again + `=` → check-out (goodbye message). Each toggles a distinct LED/buzzer pattern.

**Wrong ID:** 3 consecutive failures triggers a 15-second lockout.

**Enroll a new member (Admin only):**

1. From idle, press `D`.
2. Enter an existing **Admin** ID (e.g. `1234`) + `=` to verify.
3. Enter a new 4-digit ID + `=`.
4. Press `1` (Admin), `2` (Operator), or `3` (Viewer) to pick a role.
5. New member can now check in/out with their new ID.

> Enrolled members and check-in state are stored in RAM only — a reboot resets back to the 4 built-in members with nobody checked in. All history is still preserved on the server regardless.

---

## 6. Testing the offline queue

1. Temporarily set `SERVER_URL` to an unreachable address (or disconnect your tunnel) and reflash.
2. Log in a few times — you'll see `[QUEUE] Stored (n/10)` in Serial.
3. Restore the correct `SERVER_URL` and reflash (or bring the tunnel back).
4. Within 5 seconds, watch `[QUEUE] Flushing...` and `[QUEUE] Flushed...` — check the dashboard to confirm the queued entries arrived.

---

## Troubleshooting

- **`'EffectStep' does not name a type` build error:** Arduino auto-generates function prototypes before custom structs are defined; make sure the manual prototypes near the top of the `EffectStep` struct are intact.
- **Crash/reboot right when POSTing:** confirm `SERVER_URL` uses `https://` and that `WiFiClientSecure` + `setInsecure()` is in place in `postToServer()` — this is required for HTTPS on this ESP32 core version.
- **`SyntaxError: Unexpected token '?'` when running the server:** your Node.js version is older than 14 and doesn't support certain modern syntax — the server code avoids this now, but if you hit it elsewhere, check `node --version` and consider upgrading.
- **Dashboard shows nothing:** make sure `SERVER_URL` points at `/post` (not `/dashboard`), and that your tunnel/deployment is actually running and reachable.
