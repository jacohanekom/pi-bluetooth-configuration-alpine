#pragma once
/**
 * web_ui.hpp -- the browser-based configuration page served at GET /
 * (see main.cpp). A single self-contained HTML document (inline CSS/JS,
 * no external requests, no build step, no framework) that talks to this
 * daemon's own existing JSON routes (GET /status, POST /scan, POST
 * /connect, POST /forget, GET+POST /accounts, POST /accounts/remove,
 * POST /change-password, POST /ssh, GET+POST /ethernet, POST /time,
 * POST /relay, POST /relay-control, POST /relay-always-on) via fetch().
 *
 * Laid out as three independent sections -- WiFi, Ethernet, Relays --
 * matching main.cpp's own design: there is no wizard, no "finished
 * setup" state, and no sequencing between them. Each can be configured
 * any time, in any order; the users and system-clock cards above them
 * are device-wide utilities that don't belong to any one section. There
 * is no auto-generated login of any kind -- the "Users" card is the
 * only way a device ever gets a working SSH login at all.
 *
 * GET /status's own mustChangePassword flag (true only for root, until
 * its first POST /change-password -- see main.cpp's
 * ROOT_PASSWORD_CHANGED_FILE) switches this entire page over to a
 * single forced-password-change card, hiding every other section, until
 * it's cleared -- root's own Basic Auth credentials are a known default
 * (ROOT_PASSWORD, set once at build time) until changed, unlike any
 * account POST /accounts creates (always a fresh, user-chosen password).
 * A full page reload after a successful change is what actually forces
 * the browser to re-prompt for the (now-stale) cached credentials --
 * see the change handler's own comment for why a reload, not just
 * re-fetching, is what makes that reliable.
 *
 * Kept as one inline string rather than a directory of static assets:
 * http_server.hpp has no static-file serving (by design -- see its own
 * header comment on why this project hand-rolls only what it actually
 * needs), and a single page is little enough HTML/CSS/JS that adding a
 * static-file mechanism just to serve it wouldn't be a good trade.
 */
#include <string>

namespace webui {

inline const std::string INDEX_HTML = R"WEBUI(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Device Setup</title>
<style>
  :root {
    color-scheme: light dark;
    --bg: #f5f6f8;
    --card: #ffffff;
    --text: #1a1d21;
    --muted: #6b7280;
    --border: #e2e5e9;
    --accent: #2563eb;
    --accent-text: #ffffff;
    --ok: #16a34a;
    --warn: #d97706;
    --err: #dc2626;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #14161a;
      --card: #1e2126;
      --text: #e8eaed;
      --muted: #9aa1ab;
      --border: #2c3036;
      --accent: #4b8bf5;
      --accent-text: #0b1220;
      --ok: #4ade80;
      --warn: #fbbf24;
      --err: #f87171;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    padding: 1.25rem;
    background: var(--bg);
    color: var(--text);
    font: 15px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  }
  main { max-width: 520px; margin: 0 auto; display: flex; flex-direction: column; gap: 1rem; }
  h1 { font-size: 1.25rem; margin: 0.25rem 0 0.5rem; }
  h2 { font-size: 0.95rem; margin: 0 0 0.75rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.04em; }
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 1rem 1.1rem; }
  .row { display: flex; justify-content: space-between; align-items: center; gap: 0.75rem; padding: 0.3rem 0; }
  .row + .row { border-top: 1px solid var(--border); }
  .muted { color: var(--muted); }
  .badge { font-size: 0.8rem; padding: 0.15rem 0.55rem; border-radius: 999px; font-weight: 600; }
  .badge.connected, .badge.on { background: color-mix(in srgb, var(--ok) 18%, transparent); color: var(--ok); }
  .badge.connecting, .badge.scanning { background: color-mix(in srgb, var(--warn) 18%, transparent); color: var(--warn); }
  .badge.failed { background: color-mix(in srgb, var(--err) 18%, transparent); color: var(--err); }
  .badge.idle, .badge.off, .badge.unknown, .badge.disabled { background: color-mix(in srgb, var(--muted) 18%, transparent); color: var(--muted); }
  .switch { position: relative; display: inline-block; width: 2.6rem; height: 1.5rem; flex-shrink: 0; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .switch .slider { position: absolute; inset: 0; background: var(--border); border-radius: 999px; cursor: pointer; transition: background .15s; }
  .switch .slider::before { content: ""; position: absolute; width: 1.1rem; height: 1.1rem; left: 0.2rem; top: 0.2rem; background: var(--card); border-radius: 50%; transition: transform .15s; }
  .switch input:checked + .slider { background: var(--accent); }
  .switch input:checked + .slider::before { transform: translateX(1.1rem); }
  .switch.small { width: 2.1rem; height: 1.2rem; }
  .switch.small .slider::before { width: 0.85rem; height: 0.85rem; left: 0.17rem; top: 0.17rem; }
  .switch.small input:checked + .slider::before { transform: translateX(0.9rem); }
  ul.item-list { list-style: none; margin: 0.5rem 0 0; padding: 0; }
  ul.item-list li { padding: 0.6rem 0.2rem; }
  ul.item-list li + li { border-top: 1px solid var(--border); }
  .item-row { display: flex; justify-content: space-between; align-items: center; gap: 0.5rem; }
  .relay-label { display: flex; align-items: center; gap: 0.5rem; }
  .relay-always-on { display: flex; align-items: center; gap: 0.4rem; margin-top: 0.5rem; font-size: 0.85rem; color: var(--muted); }
  ul.networks { list-style: none; margin: 0; padding: 0; max-height: 260px; overflow-y: auto; }
  ul.networks li { display: flex; justify-content: space-between; align-items: center; padding: 0.55rem 0.2rem; cursor: pointer; border-radius: 6px; }
  ul.networks li:hover { background: var(--bg); }
  ul.networks li.selected { background: color-mix(in srgb, var(--accent) 14%, transparent); }
  .ssid { font-weight: 500; }
  .sig { color: var(--muted); font-size: 0.85rem; }
  label { display: block; font-size: 0.85rem; color: var(--muted); margin: 0.6rem 0 0.25rem; }
  input[type=text], input[type=password], input[type=number] {
    width: 100%; padding: 0.55rem 0.65rem; border-radius: 8px; border: 1px solid var(--border);
    background: var(--bg); color: var(--text); font-size: 0.95rem;
  }
  .btn-row { display: flex; gap: 0.5rem; margin-top: 0.85rem; flex-wrap: wrap; }
  button {
    appearance: none; border: 1px solid var(--border); background: var(--card); color: var(--text);
    padding: 0.55rem 0.95rem; border-radius: 8px; font-size: 0.9rem; cursor: pointer; font-weight: 500;
  }
  button.primary { background: var(--accent); color: var(--accent-text); border-color: var(--accent); }
  button.danger { color: var(--err); border-color: color-mix(in srgb, var(--err) 40%, var(--border)); }
  button:disabled { opacity: 0.5; cursor: default; }
  button.small { padding: 0.35rem 0.65rem; font-size: 0.82rem; }
  .msg { font-size: 0.88rem; margin-top: 0.6rem; }
  .msg.err { color: var(--err); }
  .msg.ok { color: var(--ok); }
  .small { font-size: 0.82rem; }
</style>
</head>
<body>
<main>
  <h1>Device Setup</h1>

  <section id="forcePasswordChangeCard" class="card" style="display:none">
    <h2>Change password required</h2>
    <div class="small muted" style="margin-bottom:0.5rem">
      This account is still using its original password. Choose a new one to continue.
    </div>
    <label for="forceCurrentPasswordInput">Current password</label>
    <input type="password" id="forceCurrentPasswordInput" autocomplete="current-password">
    <label for="forceNewPasswordInput">New password</label>
    <input type="password" id="forceNewPasswordInput" autocomplete="new-password">
    <label for="forceNewPasswordConfirmInput">Confirm new password</label>
    <input type="password" id="forceNewPasswordConfirmInput" autocomplete="new-password">
    <div class="btn-row">
      <button id="forceChangePasswordBtn" class="primary">Set new password</button>
    </div>
    <div id="forceChangePasswordMsg" class="msg"></div>
  </section>

  <div id="normalContent">
  <section class="card">
    <h2>Users</h2>
    <div class="row">
      <span>SSH access</span>
      <label class="switch">
        <input type="checkbox" id="sshEnabledInput">
        <span class="slider"></span>
      </label>
    </div>
    <div class="small muted" style="margin-top:0.5rem">
      No account is created automatically -- add one below to log in over SSH (root login is disabled).
    </div>
    <ul id="accountList" class="item-list"></ul>
    <div class="btn-row">
      <button id="addAccountBtn">Add user</button>
    </div>

    <div id="addAccountForm" style="display:none">
      <label for="newUsernameInput">Username</label>
      <input type="text" id="newUsernameInput" autocomplete="off">
      <label for="newPasswordInput">Password</label>
      <input type="password" id="newPasswordInput" autocomplete="new-password">
      <div class="btn-row">
        <button id="createAccountBtn" class="primary">Create</button>
        <button id="cancelAccountBtn">Cancel</button>
      </div>
    </div>
    <div id="accountMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2>WiFi</h2>
    <div class="row">
      <span>State</span>
      <span id="wifiState" class="badge idle">idle</span>
    </div>
    <div class="row"><span>Network</span><span id="wifiSsid" class="muted">--</span></div>
    <div class="row"><span>IP address</span><span id="wifiIp" class="muted">--</span></div>
    <div id="wifiErrorRow" class="row" style="display:none"><span class="muted">Error</span><span id="wifiError" style="color:var(--err)"></span></div>
    <div class="btn-row">
      <button id="forgetBtn" class="danger">Forget network</button>
    </div>
    <div id="wifiMsg" class="msg"></div>

    <ul id="networkList" class="networks" style="margin-top:0.75rem"><li class="muted">No scan yet</li></ul>
    <div class="btn-row">
      <button id="scanBtn">Scan</button>
      <button id="manualBtn">Enter manually</button>
    </div>

    <div id="joinForm" style="display:none">
      <label for="ssidInput">Network name (SSID)</label>
      <input type="text" id="ssidInput" autocomplete="off">
      <label for="pskInput">Password <span id="pskOptional" class="muted"></span></label>
      <input type="password" id="pskInput" autocomplete="off">
      <div class="btn-row">
        <button id="connectBtn" class="primary">Connect</button>
        <button id="cancelJoinBtn">Cancel</button>
      </div>
      <div id="connectMsg" class="msg"></div>
    </div>
  </section>

  <section class="card">
    <h2>Ethernet</h2>
    <div class="small muted" style="margin-bottom:0.5rem">
      The address this page is reachable at from either wired port (eth0/eth1, bridged).
    </div>
    <label for="ethIp">Gateway IP</label>
    <input type="text" id="ethIp">
    <label for="ethStart">DHCP range start</label>
    <input type="number" id="ethStart" min="1" max="254">
    <label for="ethEnd">DHCP range end</label>
    <input type="number" id="ethEnd" min="1" max="254">
    <div class="btn-row">
      <button id="ethSaveBtn" class="primary">Save</button>
    </div>
    <div id="ethMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2>Relays</h2>
    <div class="row">
      <span>Relay control</span>
      <label class="switch">
        <input type="checkbox" id="relayEnabledInput">
        <span class="slider"></span>
      </label>
    </div>
    <ul id="relayList" class="item-list"></ul>
    <div id="relayMsg" class="msg"></div>
  </section>

  <section class="card">
    <h2>System clock</h2>
    <div class="small muted" style="margin-bottom:0.5rem">
      This device has no battery-backed clock -- set it from this browser's own time, or pick a specific one.
    </div>
    <label for="clockInput">Date &amp; time</label>
    <input type="datetime-local" id="clockInput" step="1">
    <div class="btn-row">
      <button id="clockNowBtn">Use browser's time</button>
      <button id="clockSetBtn" class="primary">Set clock</button>
    </div>
    <div id="clockMsg" class="msg"></div>
  </section>
  </div>
</main>

<script>
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  let lastStatus = null;

  function badgeClass(state) { return "badge " + state; }

  // "YYYY-MM-DDTHH:MM:SS" in LOCAL time, what <input type=datetime-local>
  // expects/returns -- there's no timezone-aware variant of this input
  // type, so the round-trip below (fill it from Date, read it back as
  // Date) has to go through local-time fields on both ends rather than
  // toISOString() (which is UTC and would display/interpret wrong).
  function toLocalInputValue(date) {
    const pad = (n) => String(n).padStart(2, "0");
    return date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) +
      "T" + pad(date.getHours()) + ":" + pad(date.getMinutes()) + ":" + pad(date.getSeconds());
  }

  function setClockInputToNow() {
    $("clockInput").value = toLocalInputValue(new Date());
  }

  function renderNetworks(scan) {
    const list = $("networkList");
    list.innerHTML = "";
    if (!scan || scan.length === 0) {
      list.innerHTML = '<li class="muted">No networks found -- try Scan</li>';
      return;
    }
    scan.forEach((n) => {
      const li = document.createElement("li");
      const left = document.createElement("span");
      left.className = "ssid";
      left.textContent = n.ssid;
      const right = document.createElement("span");
      right.className = "sig";
      right.textContent = n.security + " · " + n.rssi + " dBm";
      li.appendChild(left);
      li.appendChild(right);
      li.addEventListener("click", () => openJoinForm(n.ssid, n.security !== "Open"));
      list.appendChild(li);
    });
  }

  function openJoinForm(ssid, needsPassword) {
    $("joinForm").style.display = "block";
    $("ssidInput").value = ssid || "";
    $("pskInput").value = "";
    $("pskOptional").textContent = needsPassword ? "" : "(open network)";
    $("connectMsg").textContent = "";
    if (ssid) $("pskInput").focus(); else $("ssidInput").focus();
  }

  function relayStateLabel(state) {
    if (state === "on") return "On";
    if (state === "off") return "Off";
    if (state === "disabled") return "Disabled";
    return "Unknown";
  }

  function renderRelays(relays, enabled) {
    // Don't fight the user mid-click -- the next poll (or the direct
    // response handling in the change listener below) re-syncs this
    // once the request actually completes either way.
    if (document.activeElement.id !== "relayEnabledInput") {
      $("relayEnabledInput").checked = !!enabled;
    }
    const list = $("relayList");
    list.innerHTML = "";
    if (!relays || relays.length === 0) {
      list.innerHTML = '<li class="muted">No relays configured</li>';
      return;
    }
    relays.forEach((r) => {
      const li = document.createElement("li");

      const top = document.createElement("div");
      top.className = "item-row";

      const left = document.createElement("span");
      left.className = "relay-label";
      const name = document.createElement("span");
      name.textContent = r.label;
      const badge = document.createElement("span");
      badge.className = badgeClass(r.state);
      badge.textContent = relayStateLabel(r.state);
      left.appendChild(name);
      left.appendChild(badge);

      const actions = document.createElement("span");
      actions.className = "btn-row";
      actions.style.marginTop = "0";
      const onBtn = document.createElement("button");
      onBtn.textContent = "On";
      onBtn.disabled = !enabled;
      onBtn.addEventListener("click", () => setRelay(r.port, "on"));
      const offBtn = document.createElement("button");
      offBtn.textContent = "Off";
      offBtn.disabled = !enabled;
      offBtn.addEventListener("click", () => setRelay(r.port, "off"));
      actions.appendChild(onBtn);
      actions.appendChild(offBtn);

      top.appendChild(left);
      top.appendChild(actions);

      const alwaysOnRow = document.createElement("label");
      alwaysOnRow.className = "relay-always-on";
      const alwaysOnSwitch = document.createElement("span");
      alwaysOnSwitch.className = "switch small";
      const alwaysOnInput = document.createElement("input");
      alwaysOnInput.type = "checkbox";
      alwaysOnInput.checked = !!r.alwaysOn;
      alwaysOnInput.addEventListener("change", () => setRelayAlwaysOn(r.port, alwaysOnInput.checked));
      const alwaysOnSlider = document.createElement("span");
      alwaysOnSlider.className = "slider";
      alwaysOnSwitch.appendChild(alwaysOnInput);
      alwaysOnSwitch.appendChild(alwaysOnSlider);
      alwaysOnRow.appendChild(alwaysOnSwitch);
      alwaysOnRow.appendChild(document.createTextNode("Always on at boot"));

      li.appendChild(top);
      li.appendChild(alwaysOnRow);
      list.appendChild(li);
    });
  }

  async function setRelay(port, state) {
    $("relayMsg").textContent = "";
    try {
      const data = await postJson("/relay", { port: port, state: state });
      renderRelays(data.relays, $("relayEnabledInput").checked);
    } catch (e) {
      $("relayMsg").textContent = e.message;
      $("relayMsg").className = "msg err";
    }
  }

  async function setRelayAlwaysOn(port, alwaysOn) {
    $("relayMsg").textContent = "";
    try {
      const data = await postJson("/relay-always-on", { port: port, alwaysOn: alwaysOn });
      renderRelays(data.relays, $("relayEnabledInput").checked);
    } catch (e) {
      $("relayMsg").textContent = e.message;
      $("relayMsg").className = "msg err";
      await refreshStatus(); // revert the checkbox to whatever's actually saved
    }
  }

  function renderAccounts(accounts, sshEnabled) {
    if (document.activeElement.id !== "sshEnabledInput") {
      $("sshEnabledInput").checked = !!sshEnabled;
    }
    const list = $("accountList");
    list.innerHTML = "";
    if (!accounts || accounts.length === 0) {
      list.innerHTML = '<li class="muted">No user accounts yet</li>';
      return;
    }
    accounts.forEach((name) => {
      const li = document.createElement("li");
      const top = document.createElement("div");
      top.className = "item-row";
      const label = document.createElement("span");
      label.textContent = name;
      const removeBtn = document.createElement("button");
      removeBtn.textContent = "Remove";
      removeBtn.className = "danger small";
      removeBtn.addEventListener("click", () => removeAccount(name));
      top.appendChild(label);
      top.appendChild(removeBtn);
      li.appendChild(top);
      list.appendChild(li);
    });
  }

  async function removeAccount(name) {
    if (!confirm('Remove user "' + name + '"?')) return;
    $("accountMsg").textContent = "";
    try {
      const data = await postJson("/accounts/remove", { username: name });
      renderAccounts(data.accounts, $("sshEnabledInput").checked);
      await refreshStatus();
    } catch (e) {
      $("accountMsg").textContent = e.message;
      $("accountMsg").className = "msg err";
    }
  }

  function renderStatus(s) {
    lastStatus = s;

    if (s.mustChangePassword) {
      $("forcePasswordChangeCard").style.display = "block";
      $("normalContent").style.display = "none";
      return;
    }
    $("forcePasswordChangeCard").style.display = "none";
    $("normalContent").style.display = "";

    const state = s.wifi.state;
    const stateEl = $("wifiState");
    stateEl.textContent = state;
    stateEl.className = badgeClass(state);
    $("wifiSsid").textContent = s.wifi.ssid || "--";
    $("wifiIp").textContent = s.wifi.ip || "--";
    if (s.wifi.error) {
      $("wifiErrorRow").style.display = "flex";
      $("wifiError").textContent = s.wifi.error;
    } else {
      $("wifiErrorRow").style.display = "none";
    }
    renderNetworks(s.scan);
    renderRelays(s.relays, s.relaysEnabled);
    renderAccounts(s.accounts, s.sshEnabled);

    if (document.activeElement.id !== "ethIp") $("ethIp").value = s.eth.ip || "";
    if (document.activeElement.id !== "ethStart") $("ethStart").value = s.eth.rangeStart || "";
    if (document.activeElement.id !== "ethEnd") $("ethEnd").value = s.eth.rangeEnd || "";
  }

  async function refreshStatus() {
    try {
      const r = await fetch("/status");
      const s = await r.json();
      renderStatus(s);
    } catch (e) { /* transient -- next poll will retry */ }
  }

  async function postJson(path, body) {
    const r = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body || {}),
    });
    const data = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(data.error || ("request failed (" + r.status + ")"));
    return data;
  }

  $("scanBtn").addEventListener("click", async () => {
    $("scanBtn").disabled = true;
    try {
      await postJson("/scan");
      setTimeout(refreshStatus, 4500);
    } finally {
      setTimeout(() => { $("scanBtn").disabled = false; }, 4500);
    }
  });

  $("manualBtn").addEventListener("click", () => openJoinForm("", true));
  $("cancelJoinBtn").addEventListener("click", () => { $("joinForm").style.display = "none"; });

  $("connectBtn").addEventListener("click", async () => {
    const ssid = $("ssidInput").value.trim();
    const psk = $("pskInput").value;
    if (!ssid) { $("connectMsg").textContent = "SSID is required"; $("connectMsg").className = "msg err"; return; }
    $("connectBtn").disabled = true;
    $("connectMsg").textContent = "Connecting…";
    $("connectMsg").className = "msg";
    try {
      await postJson("/connect", { ssid: ssid, password: psk });
      $("connectMsg").textContent = "Submitted -- watch WiFi status above.";
      $("connectMsg").className = "msg ok";
    } catch (e) {
      $("connectMsg").textContent = e.message;
      $("connectMsg").className = "msg err";
    } finally {
      $("connectBtn").disabled = false;
    }
  });

  $("forgetBtn").addEventListener("click", async () => {
    if (!confirm("Forget the configured network?")) return;
    $("forgetBtn").disabled = true;
    try {
      await postJson("/forget");
      $("wifiMsg").textContent = "Forgotten.";
      $("wifiMsg").className = "msg ok";
    } catch (e) {
      $("wifiMsg").textContent = e.message;
      $("wifiMsg").className = "msg err";
    } finally {
      $("forgetBtn").disabled = false;
    }
  });

  $("sshEnabledInput").addEventListener("change", async () => {
    const enabled = $("sshEnabledInput").checked;
    $("sshEnabledInput").disabled = true;
    try {
      await postJson("/ssh", { enabled: enabled });
      $("accountMsg").textContent = enabled ? "SSH enabled." : "SSH disabled.";
      $("accountMsg").className = "msg ok";
      await refreshStatus();
    } catch (e) {
      $("sshEnabledInput").checked = !enabled; // revert on failure
      $("accountMsg").textContent = e.message;
      $("accountMsg").className = "msg err";
    } finally {
      $("sshEnabledInput").disabled = false;
    }
  });

  $("addAccountBtn").addEventListener("click", () => {
    $("addAccountForm").style.display = "block";
    $("newUsernameInput").value = "";
    $("newPasswordInput").value = "";
    $("accountMsg").textContent = "";
    $("newUsernameInput").focus();
  });

  $("cancelAccountBtn").addEventListener("click", () => {
    $("addAccountForm").style.display = "none";
  });

  $("createAccountBtn").addEventListener("click", async () => {
    const username = $("newUsernameInput").value.trim();
    const password = $("newPasswordInput").value;
    if (!username || !password) {
      $("accountMsg").textContent = "Username and password are required";
      $("accountMsg").className = "msg err";
      return;
    }
    $("createAccountBtn").disabled = true;
    try {
      const data = await postJson("/accounts", { username: username, password: password });
      renderAccounts(data.accounts, $("sshEnabledInput").checked);
      $("addAccountForm").style.display = "none";
      $("accountMsg").textContent = "User \"" + username + "\" created.";
      $("accountMsg").className = "msg ok";
    } catch (e) {
      $("accountMsg").textContent = e.message;
      $("accountMsg").className = "msg err";
    } finally {
      $("createAccountBtn").disabled = false;
    }
  });

  $("ethSaveBtn").addEventListener("click", async () => {
    const ip = $("ethIp").value.trim();
    const rangeStart = parseInt($("ethStart").value, 10);
    const rangeEnd = parseInt($("ethEnd").value, 10);
    $("ethSaveBtn").disabled = true;
    try {
      await postJson("/ethernet", { ip: ip, rangeStart: rangeStart, rangeEnd: rangeEnd });
      $("ethMsg").textContent = "Saved.";
      $("ethMsg").className = "msg ok";
    } catch (e) {
      $("ethMsg").textContent = e.message;
      $("ethMsg").className = "msg err";
    } finally {
      $("ethSaveBtn").disabled = false;
    }
  });

  $("relayEnabledInput").addEventListener("change", async () => {
    const enabled = $("relayEnabledInput").checked;
    $("relayEnabledInput").disabled = true;
    try {
      await postJson("/relay-control", { enabled: enabled });
      $("relayMsg").textContent = enabled ? "Relay control enabled." : "Relay control disabled.";
      $("relayMsg").className = "msg ok";
      await refreshStatus();
    } catch (e) {
      $("relayEnabledInput").checked = !enabled; // revert on failure
      $("relayMsg").textContent = e.message;
      $("relayMsg").className = "msg err";
    } finally {
      $("relayEnabledInput").disabled = false;
    }
  });

  $("clockNowBtn").addEventListener("click", setClockInputToNow);

  $("clockSetBtn").addEventListener("click", async () => {
    const value = $("clockInput").value;
    if (!value) { $("clockMsg").textContent = "Pick a date and time first"; $("clockMsg").className = "msg err"; return; }
    // new Date("...") parses a bare "YYYY-MM-DDTHH:MM:SS" (no timezone
    // suffix) as LOCAL time, matching what the input actually shows --
    // .getTime() then gives the correct UTC instant regardless of this
    // browser's own timezone.
    const unixTime = Math.floor(new Date(value).getTime() / 1000);
    $("clockSetBtn").disabled = true;
    try {
      await postJson("/time", { unixTime: unixTime });
      $("clockMsg").textContent = "Clock set.";
      $("clockMsg").className = "msg ok";
    } catch (e) {
      $("clockMsg").textContent = e.message;
      $("clockMsg").className = "msg err";
    } finally {
      $("clockSetBtn").disabled = false;
    }
  });

  $("forceChangePasswordBtn").addEventListener("click", async () => {
    const current = $("forceCurrentPasswordInput").value;
    const next = $("forceNewPasswordInput").value;
    const confirmNext = $("forceNewPasswordConfirmInput").value;
    if (!current || !next) {
      $("forceChangePasswordMsg").textContent = "Current and new password are both required";
      $("forceChangePasswordMsg").className = "msg err";
      return;
    }
    if (next !== confirmNext) {
      $("forceChangePasswordMsg").textContent = "New passwords don't match";
      $("forceChangePasswordMsg").className = "msg err";
      return;
    }
    $("forceChangePasswordBtn").disabled = true;
    $("forceChangePasswordMsg").textContent = "";
    try {
      await postJson("/change-password", {
        username: (lastStatus && lastStatus.loggedInAs) || "root",
        currentPassword: current,
        newPassword: next,
      });
      $("forceChangePasswordMsg").textContent = "Password changed -- reloading to log in again…";
      $("forceChangePasswordMsg").className = "msg ok";
      // A plain fetch() retry would just keep sending the browser's
      // still-cached (now-stale) credentials and fail forever -- only a
      // full top-level navigation reliably makes a browser drop those
      // and show its native Basic Auth prompt again after a 401, the
      // way GET / is about to return now that the password's changed.
      setTimeout(() => { location.reload(); }, 1000);
    } catch (e) {
      $("forceChangePasswordMsg").textContent = e.message;
      $("forceChangePasswordMsg").className = "msg err";
      $("forceChangePasswordBtn").disabled = false;
    }
  });

  setClockInputToNow();
  refreshStatus();
  setInterval(refreshStatus, 3000);
})();
</script>
</body>
</html>
)WEBUI";

} // namespace webui
