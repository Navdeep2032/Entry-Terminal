const express = require("express");
const Database = require("better-sqlite3");

const app = express();
app.use(express.json());

// ---- Persistent storage (SQLite file on disk) ----
const db = new Database("logs.db");
db.exec(`
  CREATE TABLE IF NOT EXISTS access_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at TEXT NOT NULL,      -- when the SERVER received this record
    device_timestamp TEXT,          -- the ESP32's own timestamp (NTP or uptime fallback)
    member_id TEXT,
    member_name TEXT,
    role INTEGER,
    result TEXT NOT NULL            -- GRANTED / DENIED / LOCKOUT / ENROLL
  )
`);

const insertLog = db.prepare(`
  INSERT INTO access_logs (received_at, device_timestamp, member_id, member_name, role, result)
  VALUES (?, ?, ?, ?, ?, ?)
`);

const ROLE_NAMES = { 0: "None", 1: "Admin", 2: "Operator", 3: "Viewer" };

// Plain helper instead of the ?? operator, which needs Node 14+ and breaks
// on older Node installs (older Ubuntu/LTS setups often ship Node 12).
function orDefault(v, fallback) {
  return v === undefined || v === null ? fallback : v;
}

// This is the endpoint your ESP32 POSTs each JSON record to.
app.post("/post", (req, res) => {
  const body = req.body || {};
  const receivedAt = new Date().toISOString();

  insertLog.run(
    receivedAt,
    body.timestamp || null,
    body.id || body.new_id || null,
    body.name || null,
    orDefault(body.role, null),
    body.result || "UNKNOWN",
  );

  console.log(
    `[LOG] ${body.result} - id=${body.id || body.new_id || "?"} @ ${receivedAt}`,
  );
  res.status(200).json({ status: "ok" });
});

// Raw JSON, useful for debugging or feeding into something else
app.get("/logs", (req, res) => {
  const rows = db
    .prepare("SELECT * FROM access_logs ORDER BY id DESC LIMIT 200")
    .all();
  res.json({ count: rows.length, logs: rows });
});

// Human-readable dashboard: who logged in, and when
app.get("/dashboard", (req, res) => {
  // Pair up CHECK_IN / CHECK_OUT events per member into sessions
  const checkEvents = db
    .prepare(
      "SELECT * FROM access_logs WHERE result IN ('CHECK_IN','CHECK_OUT') ORDER BY id ASC",
    )
    .all();

  const open = {}; // member_id -> { name, role, checkIn }
  const sessions = []; // completed or currently-open sessions, most recent first

  for (const r of checkEvents) {
    if (r.result === "CHECK_IN") {
      open[r.member_id] = {
        name: r.member_name,
        role: r.role,
        checkIn: r.device_timestamp || r.received_at,
      };
    } else if (r.result === "CHECK_OUT") {
      const o = open[r.member_id];
      sessions.push({
        member_id: r.member_id,
        name: r.member_name || (o && o.name) || "-",
        role: orDefault(r.role, o && o.role),
        checkIn: (o && o.checkIn) || "-",
        checkOut: r.device_timestamp || r.received_at,
      });
      delete open[r.member_id];
    }
  }
  // Anyone left in `open` is still checked in right now
  const stillIn = Object.entries(open).map(([id, o]) => ({
    member_id: id,
    ...o,
  }));
  sessions.reverse(); // most recent completed session first

  function fmtDuration(startStr, endStr) {
    const start = new Date(startStr),
      end = new Date(endStr);
    if (isNaN(start) || isNaN(end)) return "-";
    const mins = Math.round((end - start) / 60000);
    if (mins < 1) return "<1 min";
    if (mins < 60) return `${mins} min`;
    return `${Math.floor(mins / 60)}h ${mins % 60}m`;
  }

  const stillInHtml = stillIn.length
    ? stillIn
        .map(
          (o) => `<tr class="checkedin">
        <td>${o.member_id}</td><td>${o.name || "-"}</td>
        <td>${orDefault(ROLE_NAMES[o.role], "-")}</td><td>${o.checkIn}</td>
      </tr>`,
        )
        .join("\n")
    : `<tr><td colspan="4" style="text-align:center;color:#666">Nobody currently checked in</td></tr>`;

  const sessionsHtml = sessions
    .map(
      (s) => `<tr>
      <td>${s.member_id}</td><td>${s.name}</td><td>${orDefault(ROLE_NAMES[s.role], "-")}</td>
      <td>${s.checkIn}</td><td>${s.checkOut}</td><td>${fmtDuration(s.checkIn, s.checkOut)}</td>
    </tr>`,
    )
    .join("\n");

  // Full recent activity feed, including DENIED/LOCKOUT/ENROLL
  const rows = db
    .prepare("SELECT * FROM access_logs ORDER BY id DESC LIMIT 200")
    .all();
  const rowsHtml = rows
    .map((r) => {
      const roleName = orDefault(ROLE_NAMES[r.role], "-");
      const cls = r.result.toLowerCase();
      return `<tr class="${cls}">
      <td>${r.received_at}</td>
      <td>${r.device_timestamp || "-"}</td>
      <td>${r.member_id || "-"}</td>
      <td>${r.member_name || "-"}</td>
      <td>${roleName}</td>
      <td>${r.result}</td>
    </tr>`;
    })
    .join("\n");

  res.send(`<!DOCTYPE html>
<html>
<head>
  <title>Access Log Dashboard</title>
  <meta http-equiv="refresh" content="5">
  <style>
    body { font-family: sans-serif; background:#111; color:#eee; padding:24px; }
    h1 { color:#0ff; margin-bottom:4px; }
    h2 { color:#0ff; margin-top:32px; font-size:18px; }
    .count { color:#888; margin-bottom:12px; }
    table { width:100%; border-collapse: collapse; }
    th, td { padding:8px 12px; border-bottom:1px solid #333; text-align:left; }
    th { background:#1a1a1a; color:#0ff; }
    tr.granted, tr.check_in  { background: rgba(0,255,0,0.07); }
    tr.denied               { background: rgba(255,0,0,0.07); }
    tr.lockout              { background: rgba(255,150,0,0.1); }
    tr.enroll               { background: rgba(0,150,255,0.1); }
    tr.check_out            { background: rgba(255,255,0,0.05); }
    tr.checkedin            { background: rgba(0,255,0,0.12); }
  </style>
</head>
<body>
  <h1>Access Log Dashboard</h1>

  <h2>Currently Checked In (${stillIn.length})</h2>
  <table>
    <tr><th>ID</th><th>Name</th><th>Role</th><th>Check-In Time</th></tr>
    ${stillInHtml}
  </table>

  <h2>Check-In / Check-Out Sessions</h2>
  <table>
    <tr><th>ID</th><th>Name</th><th>Role</th><th>Check-In</th><th>Check-Out</th><th>Duration</th></tr>
    ${sessionsHtml || '<tr><td colspan="6" style="text-align:center;color:#666">No completed sessions yet</td></tr>'}
  </table>

  <h2>Recent Activity (all events)</h2>
  <div class="count">${rows.length} records - auto-refreshes every 5s</div>
  <table>
    <tr><th>Received At</th><th>Device Time</th><th>ID</th><th>Name</th><th>Role</th><th>Result</th></tr>
    ${rowsHtml}
  </table>
</body>
</html>`);
});

app.get("/", (req, res) => res.redirect("/dashboard"));

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`Server listening on port ${PORT}`);
});
