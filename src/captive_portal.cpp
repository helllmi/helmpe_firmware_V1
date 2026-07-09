#include <Arduino.h>
#include "captive_portal.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include <SPI.h>
#include "wifi_creds.h"
#include "mqtt_config.h"
// Pour récupérer l'état du device
#include "state_machine.h"
#include "mqtt_transport.h"
#include "storage.h"
#include "serial_comm.h"
#include "boot_mode.h"

// ============================================================================
//  ÉTAT INTERNE
// ============================================================================
static bool portalActive = false;
static WebServer server(80);
static DNSServer dnsServer;
static uint32_t lastActivityMs = 0;

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

// ============================================================================
//  PAGE HTML — UI avec auto-refresh des données via /api/state
// ============================================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>HEELPMEE Config</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: #0d0d0d;
      color: #e8e8e8;
      font-family: 'Courier New', monospace;
      padding: 1.5rem;
      max-width: 600px;
      margin: 0 auto;
    }
    h1 {
      color: #00ff99;
      letter-spacing: 0.2em;
      font-size: 1.5rem;
      text-align: center;
      text-shadow: 0 0 12px #00ff9966;
      margin-bottom: 0.5rem;
    }
    .subtitle {
      text-align: center;
      color: #888;
      letter-spacing: 0.1em;
      font-size: 0.8rem;
      margin-bottom: 2rem;
    }
    .card {
      background: #1a1a1a;
      border: 1px solid #333;
      border-radius: 6px;
      padding: 1rem;
      margin-bottom: 1rem;
    }
    .card-title {
      color: #00ff99;
      font-size: 0.8rem;
      letter-spacing: 0.2em;
      text-transform: uppercase;
      margin-bottom: 0.8rem;
      border-bottom: 1px solid #333;
      padding-bottom: 0.5rem;
    }
    .row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 0.4rem 0;
      font-size: 0.9rem;
    }
    .label { color: #888; }
    .value { color: #e8e8e8; font-weight: bold; }
    .value.ok { color: #00ff99; }
    .value.warn { color: #ffaa00; }
    .value.err { color: #ff4455; }
    .countdown {
      text-align: center;
      color: #4488ff;
      font-size: 0.85rem;
      margin-top: 1rem;
      letter-spacing: 0.1em;
    }
    .menu {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 0.8rem;
      margin-top: 1.5rem;
    }
    .menu a {
      background: transparent;
      border: 1px solid #4488ff;
      color: #4488ff;
      text-decoration: none;
      text-align: center;
      padding: 0.8rem;
      font-size: 0.85rem;
      letter-spacing: 0.15em;
      text-transform: uppercase;
      border-radius: 4px;
      transition: all 0.2s;
    }
    .menu a:hover { background: #4488ff22; box-shadow: 0 0 12px #4488ff44; }
  </style>
</head>
<body>
  <h1>HEELPMEE</h1>
  <div class="subtitle">PORTAIL DE CONFIGURATION</div>

  <div class="card">
    <div class="card-title">État du device</div>
    <div class="row"><span class="label">FSM</span><span class="value" id="fsm">…</span></div>
    <div class="row"><span class="label">Batterie</span><span class="value" id="bat">…</span></div>
    <div class="row"><span class="label">Signal LTE</span><span class="value" id="sig">…</span></div>
    <div class="row"><span class="label">Transport</span><span class="value" id="tr">…</span></div>
    <div class="row"><span class="label">MQTT</span><span class="value" id="mqtt">…</span></div>
    <div class="row"><span class="label">Uptime</span><span class="value" id="up">…</span></div>
  </div>

  <div class="card">
    <div class="card-title">Identification</div>
    <div class="row"><span class="label">Device ID</span><span class="value" id="dev">…</span></div>
    <div class="row"><span class="label">Firmware</span><span class="value" id="fw">…</span></div>
    <div class="row"><span class="label">IP LTE</span><span class="value" id="ip">…</span></div>
  </div>

  <div class="card">
    <div class="card-title">Stockage</div>
    <div class="row"><span class="label">Alertes bufferisées</span><span class="value" id="buf">…</span></div>
    <div class="row"><span class="label">Audios sur SD</span><span class="value" id="aud">…</span></div>
  </div>

  <div class="menu">
    <a href="/wifi">WiFi Manager</a>
    <a href="/mqtt">Config MQTT</a>
    <a href="/audios">Audios SD</a>
    <a href="/reboot">Reboot</a>
  </div>

  <div class="countdown" id="cd">Auto-shutdown : --</div>

  <script>
    async function refresh() {
      try {
        const r = await fetch('/api/state');
        const d = await r.json();
        document.getElementById('fsm').textContent  = d.fsm;
        document.getElementById('fsm').className    = 'value ' + (d.fsm === 'ACTION' ? 'err' : 'ok');
        document.getElementById('bat').textContent  = d.battery + '%';
        document.getElementById('sig').textContent  = d.rssi + ' dBm';
        document.getElementById('tr').textContent   = d.transport;
        document.getElementById('mqtt').textContent = d.mqtt_connected ? 'CONNECTED' : 'DISCONNECTED';
        document.getElementById('mqtt').className   = 'value ' + (d.mqtt_connected ? 'ok' : 'err');
        document.getElementById('up').textContent   = formatUptime(d.uptime_s);
        document.getElementById('dev').textContent  = d.device_id;
        document.getElementById('fw').textContent   = d.firmware;
        document.getElementById('ip').textContent   = d.ip || 'n/a';
        document.getElementById('buf').textContent  = d.pending_alerts;
        document.getElementById('aud').textContent  = d.audio_count;
        document.getElementById('cd').textContent   = 'Auto-shutdown dans ' + Math.ceil(d.shutdown_in_s) + 's';
      } catch (e) {
        console.error('refresh failed', e);
      }
    }
    function formatUptime(s) {
      const h = Math.floor(s / 3600);
      const m = Math.floor((s % 3600) / 60);
      const sec = s % 60;
      return h + 'h ' + m + 'm ' + sec + 's';
    }
    refresh();
    setInterval(refresh, 5000);
  </script>
</body>
</html>
)rawliteral";
// Page HTML pour la configuration  wifi
static const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WiFi Manager — HEELPMEE</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: #0d0d0d; color: #e8e8e8; font-family: 'Courier New', monospace;
           padding: 1.5rem; max-width: 600px; margin: 0 auto; }
    h1 { color: #00ff99; letter-spacing: 0.2em; font-size: 1.3rem; text-align: center;
         text-shadow: 0 0 12px #00ff9966; margin-bottom: 1.5rem; }
    .back { color: #888; font-size: 0.8rem; text-decoration: none; letter-spacing: 0.1em; }
    .back:hover { color: #4488ff; }
    .card { background: #1a1a1a; border: 1px solid #333; border-radius: 6px;
            padding: 1rem; margin-bottom: 1rem; }
    .card-title { color: #00ff99; font-size: 0.8rem; letter-spacing: 0.2em;
                  text-transform: uppercase; margin-bottom: 0.8rem;
                  border-bottom: 1px solid #333; padding-bottom: 0.5rem; }
    .row { display: flex; justify-content: space-between; padding: 0.4rem 0; font-size: 0.9rem; }
    .label { color: #888; }
    .value { color: #e8e8e8; font-weight: bold; }
    .input-group { margin-bottom: 1rem; }
    .input-group label { display: block; color: #888; font-size: 0.8rem;
                         margin-bottom: 0.4rem; letter-spacing: 0.1em; text-transform: uppercase; }
    input[type=text], input[type=password], select {
      width: 100%; background: #0d0d0d; border: 1px solid #333; color: #e8e8e8;
      padding: 0.6rem; font-family: inherit; font-size: 0.9rem; border-radius: 4px;
    }
    input:focus, select:focus { outline: none; border-color: #00ff99; }
    button { background: transparent; border: 1px solid #4488ff; color: #4488ff;
             padding: 0.7rem 1.2rem; font-family: inherit; font-size: 0.85rem;
             letter-spacing: 0.15em; text-transform: uppercase; border-radius: 4px;
             cursor: pointer; transition: all 0.2s; width: 100%; }
    button:hover { background: #4488ff22; box-shadow: 0 0 12px #4488ff44; }
    button:disabled { opacity: 0.4; cursor: default; }
    button.danger { border-color: #ff4455; color: #ff4455; }
    button.danger:hover { background: #ff445522; box-shadow: 0 0 12px #ff445544; }
    .scan-list { max-height: 200px; overflow-y: auto; margin-top: 0.5rem; }
    .scan-item { padding: 0.5rem; cursor: pointer; border-bottom: 1px solid #222;
                 display: flex; justify-content: space-between; font-size: 0.85rem; }
    .scan-item:hover { background: #1a2a3a; }
    .scan-item .rssi { color: #888; }
    .msg { text-align: center; padding: 0.5rem; margin-top: 1rem; border-radius: 4px;
           font-size: 0.85rem; display: none; }
    .msg.ok { background: #00ff9922; color: #00ff99; display: block; }
    .msg.err { background: #ff445522; color: #ff4455; display: block; }
  </style>
</head>
<body>
  <h1>WIFI MANAGER</h1>
  <a class="back" href="/">← Retour</a>

  <div class="card">
    <div class="card-title">WiFi enregistré</div>
    <div class="row"><span class="label">SSID</span><span class="value" id="curSsid">…</span></div>
    <button class="danger" onclick="clearCreds()" id="clearBtn" style="margin-top: 1rem;">Effacer</button>
  </div>

  <div class="card">
    <div class="card-title">Nouveau WiFi</div>
    <button onclick="scanWifi()" id="scanBtn">Scanner les WiFi</button>
    <div class="scan-list" id="scanList"></div>

    <div class="input-group" style="margin-top: 1rem;">
      <label>SSID</label>
      <input type="text" id="ssid" placeholder="Nom du réseau">
    </div>
    <div class="input-group">
      <label>Mot de passe</label>
      <input type="password" id="pwd" placeholder="(vide si réseau ouvert)">
    </div>
    <button onclick="saveCreds()">Enregistrer</button>
    <div class="msg" id="msg"></div>
  </div>

  <script>
    async function loadCurrent() {
      try {
        const r = await fetch('/api/wifi/get');
        const d = await r.json();
        document.getElementById('curSsid').textContent = d.ssid || '(aucun)';
      } catch(e) {}
    }

    async function scanWifi() {
      const btn = document.getElementById('scanBtn');
      btn.disabled = true; btn.textContent = 'Scan...';
      try {
        const r = await fetch('/api/wifi/scan');
        const d = await r.json();
        const list = document.getElementById('scanList');
        list.innerHTML = '';
        if (d.networks.length === 0) {
          list.innerHTML = '<div class="scan-item">Aucun réseau détecté</div>';
        } else {
          d.networks.forEach(n => {
            const item = document.createElement('div');
            item.className = 'scan-item';
            item.innerHTML = '<span>' + n.ssid + (n.secure ? ' 🔒' : '') + '</span>' +
                             '<span class="rssi">' + n.rssi + ' dBm</span>';
            item.onclick = () => document.getElementById('ssid').value = n.ssid;
            list.appendChild(item);
          });
        }
      } catch(e) {
        document.getElementById('scanList').innerHTML = '<div class="scan-item">Erreur de scan</div>';
      }
      btn.disabled = false; btn.textContent = 'Scanner les WiFi';
    }

    async function saveCreds() {
      const ssid = document.getElementById('ssid').value.trim();
      const pwd = document.getElementById('pwd').value;
      const msg = document.getElementById('msg');

      if (!ssid) {
        msg.className = 'msg err';
        msg.textContent = 'SSID obligatoire';
        return;
      }

      msg.className = '';
      msg.textContent = '';

      try {
        const formData = new FormData();
        formData.append('ssid', ssid);
        formData.append('pwd', pwd);
        const r = await fetch('/api/wifi/set', { method: 'POST', body: formData });
        const d = await r.json();
        if (d.ok) {
          msg.className = 'msg ok';
          msg.textContent = '✓ Enregistré : ' + ssid;
          document.getElementById('ssid').value = '';
          document.getElementById('pwd').value = '';
          loadCurrent();
        } else {
          msg.className = 'msg err';
          msg.textContent = '✗ Erreur d\'enregistrement';
        }
      } catch(e) {
        msg.className = 'msg err';
        msg.textContent = '✗ Erreur réseau';
      }
    }

    async function clearCreds() {
      if (!confirm('Effacer le WiFi enregistré ?')) return;
      try {
        await fetch('/api/wifi/clear', { method: 'POST' });
        loadCurrent();
      } catch(e) {}
    }

    loadCurrent();
  </script>
</body>
</html>
)rawliteral";

// Page HTML pour la gestion des audios sur SD (liste, téléchargement, suppression)
static const char AUDIOS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Audios SD — HEELPMEE</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: #0d0d0d; color: #e8e8e8; font-family: 'Courier New', monospace;
           padding: 1.5rem; max-width: 600px; margin: 0 auto; }
    h1 { color: #00ff99; letter-spacing: 0.2em; font-size: 1.3rem; text-align: center;
         text-shadow: 0 0 12px #00ff9966; margin-bottom: 1rem; }
    .back { color: #888; font-size: 0.8rem; text-decoration: none; letter-spacing: 0.1em; }
    .back:hover { color: #4488ff; }
    .card { background: #1a1a1a; border: 1px solid #333; border-radius: 6px;
            padding: 1rem; margin-bottom: 1rem; }
    .card-title { color: #00ff99; font-size: 0.8rem; letter-spacing: 0.2em;
                  text-transform: uppercase; margin-bottom: 0.8rem;
                  border-bottom: 1px solid #333; padding-bottom: 0.5rem; }
    .stats { display: flex; justify-content: space-between; font-size: 0.85rem;
             margin-bottom: 0.5rem; }
    .stats .label { color: #888; }
    .stats .value { color: #e8e8e8; font-weight: bold; }
    .file-item { background: #0d0d0d; border: 1px solid #222; border-radius: 4px;
                 padding: 0.7rem; margin-bottom: 0.5rem;
                 display: flex; flex-direction: column; gap: 0.5rem; }
    .file-info { display: flex; justify-content: space-between; align-items: center;
                 font-size: 0.85rem; }
    .file-name { color: #e8e8e8; font-weight: bold; }
    .file-size { color: #888; font-size: 0.75rem; }
    .file-actions { display: flex; gap: 0.5rem; }
    .btn-action { flex: 1; background: transparent; border: 1px solid; padding: 0.4rem;
                  font-family: inherit; font-size: 0.75rem; letter-spacing: 0.1em;
                  text-transform: uppercase; border-radius: 4px; cursor: pointer;
                  text-decoration: none; text-align: center; transition: all 0.2s; }
    .btn-dl { border-color: #4488ff; color: #4488ff; }
    .btn-dl:hover { background: #4488ff22; }
    .btn-rm { border-color: #ff4455; color: #ff4455; }
    .btn-rm:hover { background: #ff445522; }
    button { background: transparent; padding: 0.7rem 1.2rem; font-family: inherit;
             font-size: 0.85rem; letter-spacing: 0.15em; text-transform: uppercase;
             border-radius: 4px; cursor: pointer; transition: all 0.2s; width: 100%; }
    .btn-clear { border: 1px solid #ff4455; color: #ff4455; }
    .btn-clear:hover { background: #ff445522; box-shadow: 0 0 12px #ff445544; }
    .empty { text-align: center; color: #555; padding: 2rem; font-size: 0.9rem;
             letter-spacing: 0.1em; }
    .msg { text-align: center; padding: 0.5rem; margin-top: 1rem; border-radius: 4px;
           font-size: 0.85rem; display: none; }
    .msg.ok { background: #00ff9922; color: #00ff99; display: block; }
    .msg.err { background: #ff445522; color: #ff4455; display: block; }
    .loading { text-align: center; color: #888; padding: 1rem; font-size: 0.85rem; }
  </style>
</head>
<body>
  <h1>AUDIOS SD</h1>
  <a class="back" href="/">← Retour</a>

  <div class="card">
    <div class="card-title">Stockage</div>
    <div class="stats"><span class="label">Fichiers</span><span class="value" id="count">…</span></div>
    <div class="stats"><span class="label">Espace utilisé</span><span class="value" id="used">…</span></div>
    <div class="stats"><span class="label">Espace libre</span><span class="value" id="free">…</span></div>
  </div>

  <div class="card">
    <div class="card-title">Fichiers d'alertes</div>
    <div id="list"><div class="loading">Chargement…</div></div>
  </div>

  <button class="btn-clear" onclick="clearAll()">Tout effacer</button>
  <div class="msg" id="msg"></div>
  <button class="btn-scan" onclick="forceLte()" style="margin-top: 0.5rem;">
  Forcer le retour en LTE
</button>

  <script>
    async function loadList() {
      try {
        const r = await fetch('/api/audios/list');
        const d = await r.json();

        document.getElementById('count').textContent = d.count;
        document.getElementById('used').textContent  = formatBytes(d.used);
        document.getElementById('free').textContent  = formatBytes(d.free);

        const list = document.getElementById('list');
        if (d.files.length === 0) {
          list.innerHTML = '<div class="empty">Aucun enregistrement</div>';
          return;
        }

        list.innerHTML = '';
        d.files.forEach(f => {
          const item = document.createElement('div');
          item.className = 'file-item';
          item.innerHTML =
            '<div class="file-info">' +
              '<span class="file-name">' + f.name + '</span>' +
              '<span class="file-size">' + formatBytes(f.size) + '</span>' +
            '</div>' +
            '<div class="file-actions">' +
              '<a class="btn-action btn-dl" href="/api/audios/download?f=' + encodeURIComponent(f.name) + '">Télécharger</a>' +
              '<button class="btn-action btn-rm" onclick="rmFile(\'' + f.name + '\')">Supprimer</button>' +
            '</div>';
          list.appendChild(item);
        });
      } catch(e) {
        document.getElementById('list').innerHTML = '<div class="empty">Erreur de chargement</div>';
      }
    }
      async function forceLte() {
  if (!confirm('Revenir au mode LTE primaire ?\n\nLe device va redémarrer.')) return;
  try {
    await fetch('/api/wifi/force_lte', { method: 'POST' });
    alert('Reboot en cours... Reconnecte-toi dans 30s si besoin.');
  } catch(e) {}
}

    async function rmFile(name) {
      if (!confirm('Supprimer ' + name + ' ?')) return;
      try {
        const fd = new FormData(); fd.append('f', name);
        const r = await fetch('/api/audios/delete', { method: 'POST', body: fd });
        const d = await r.json();
        showMsg(d.ok ? '✓ Supprimé : ' + name : '✗ Erreur de suppression', d.ok);
        loadList();
      } catch(e) { showMsg('✗ Erreur réseau', false); }
    }

    async function clearAll() {
      if (!confirm('Effacer TOUS les fichiers audio ?\n\nCette action est irréversible.')) return;
      try {
        const r = await fetch('/api/audios/clear', { method: 'POST' });
        const d = await r.json();
        showMsg(d.ok ? '✓ ' + d.deleted + ' fichier(s) supprimé(s)' : '✗ Erreur', d.ok);
        loadList();
      } catch(e) { showMsg('✗ Erreur réseau', false); }
    }

    function showMsg(text, ok) {
      const m = document.getElementById('msg');
      m.className = 'msg ' + (ok ? 'ok' : 'err');
      m.textContent = text;
      setTimeout(() => { m.className = 'msg'; m.textContent = ''; }, 4000);
    }

    function formatBytes(b) {
      if (b < 1024) return b + ' B';
      if (b < 1024*1024) return (b/1024).toFixed(1) + ' KB';
      return (b/1024/1024).toFixed(2) + ' MB';
    }

    loadList();
  </script>
</body>
</html>
)rawliteral";
static const char MQTT_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Config MQTT — HEELPMEE</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: #0d0d0d; color: #e8e8e8; font-family: 'Courier New', monospace;
           padding: 1.5rem; max-width: 600px; margin: 0 auto; }
    h1 { color: #00ff99; letter-spacing: 0.2em; font-size: 1.3rem; text-align: center;
         text-shadow: 0 0 12px #00ff9966; margin-bottom: 1rem; }
    .back { color: #888; font-size: 0.8rem; text-decoration: none; letter-spacing: 0.1em; }
    .back:hover { color: #4488ff; }
    .card { background: #1a1a1a; border: 1px solid #333; border-radius: 6px;
            padding: 1rem; margin-bottom: 1rem; }
    .card-title { color: #00ff99; font-size: 0.8rem; letter-spacing: 0.2em;
                  text-transform: uppercase; margin-bottom: 0.8rem;
                  border-bottom: 1px solid #333; padding-bottom: 0.5rem; }
    .row { display: flex; justify-content: space-between; padding: 0.4rem 0; font-size: 0.85rem; }
    .label { color: #888; }
    .value { color: #e8e8e8; font-weight: bold; word-break: break-all; }
    .badge { display: inline-block; padding: 0.2rem 0.5rem; border-radius: 3px;
             font-size: 0.7rem; letter-spacing: 0.1em; margin-left: 0.5rem; }
    .badge.custom { background: #4488ff22; color: #4488ff; border: 1px solid #4488ff; }
    .badge.default { background: #88888822; color: #888; border: 1px solid #888; }
    .input-group { margin-bottom: 1rem; }
    .input-group label { display: block; color: #888; font-size: 0.8rem;
                         margin-bottom: 0.4rem; letter-spacing: 0.1em; text-transform: uppercase; }
    input[type=text], input[type=number] {
      width: 100%; background: #0d0d0d; border: 1px solid #333; color: #e8e8e8;
      padding: 0.6rem; font-family: inherit; font-size: 0.9rem; border-radius: 4px;
    }
    input:focus { outline: none; border-color: #00ff99; }
    button { background: transparent; padding: 0.7rem 1.2rem; font-family: inherit;
             font-size: 0.85rem; letter-spacing: 0.15em; text-transform: uppercase;
             border-radius: 4px; cursor: pointer; transition: all 0.2s; width: 100%; }
    .btn-save { border: 1px solid #00ff99; color: #00ff99; }
    .btn-save:hover { background: #00ff9922; box-shadow: 0 0 12px #00ff9944; }
    .btn-reset { border: 1px solid #ffaa00; color: #ffaa00; margin-top: 0.5rem; }
    .btn-reset:hover { background: #ffaa0022; box-shadow: 0 0 12px #ffaa0044; }
    .warning { background: #ffaa0011; border: 1px solid #ffaa0044; padding: 0.7rem;
               border-radius: 4px; color: #ffaa00; font-size: 0.8rem; margin-top: 1rem;
               line-height: 1.4; }
    .msg { text-align: center; padding: 0.5rem; margin-top: 1rem; border-radius: 4px;
           font-size: 0.85rem; display: none; }
    .msg.ok { background: #00ff9922; color: #00ff99; display: block; }
    .msg.err { background: #ff445522; color: #ff4455; display: block; }
  </style>
</head>
<body>
  <h1>CONFIG MQTT</h1>
  <a class="back" href="/">← Retour</a>

  <div class="card">
    <div class="card-title">Configuration active <span id="badge"></span></div>
    <div class="row"><span class="label">Broker</span><span class="value" id="cur-broker">…</span></div>
    <div class="row"><span class="label">Port</span><span class="value" id="cur-port">…</span></div>
    <div class="row"><span class="label">Client ID</span><span class="value" id="cur-clientid">…</span></div>
  </div>

  <div class="card">
    <div class="card-title">Modifier</div>
    <div class="input-group">
      <label>Broker (host ou IP)</label>
      <input type="text" id="broker" placeholder="ex: broker.hivemq.com">
    </div>
    <div class="input-group">
      <label>Port</label>
      <input type="number" id="port" placeholder="1883" min="1" max="65535">
    </div>
    <div class="input-group">
      <label>Client ID</label>
      <input type="text" id="clientid" placeholder="ex: esp32-s3-test-01">
    </div>
    <button class="btn-save" onclick="save()">Enregistrer</button>
    <button class="btn-reset" onclick="resetDefaults()">Réinitialiser (défaut)</button>
    <div class="msg" id="msg"></div>
    <div class="warning">
      ⚠ Les changements prennent effet au prochain redémarrage du device.
      Utilisez le menu "Reboot" pour redémarrer immédiatement.
    </div>
  </div>

  <script>
    async function loadCurrent() {
      try {
        const r = await fetch('/api/mqtt/get');
        const d = await r.json();
        document.getElementById('cur-broker').textContent   = d.broker;
        document.getElementById('cur-port').textContent     = d.port;
        document.getElementById('cur-clientid').textContent = d.clientId;
        document.getElementById('broker').value             = d.broker;
        document.getElementById('port').value               = d.port;
        document.getElementById('clientid').value           = d.clientId;

        const badge = document.getElementById('badge');
        if (d.custom) {
          badge.className = 'badge custom';
          badge.textContent = 'CUSTOM';
        } else {
          badge.className = 'badge default';
          badge.textContent = 'DEFAULT';
        }
      } catch(e) {}
    }

    async function save() {
      const broker   = document.getElementById('broker').value.trim();
      const port     = document.getElementById('port').value.trim();
      const clientId = document.getElementById('clientid').value.trim();
      const msg = document.getElementById('msg');

      if (!broker || !port || !clientId) {
        showMsg('Tous les champs sont obligatoires', false);
        return;
      }

      const portNum = parseInt(port);
      if (isNaN(portNum) || portNum < 1 || portNum > 65535) {
        showMsg('Port invalide (1-65535)', false);
        return;
      }

      try {
        const fd = new FormData();
        fd.append('broker', broker);
        fd.append('port', port);
        fd.append('clientid', clientId);
        const r = await fetch('/api/mqtt/set', { method: 'POST', body: fd });
        const d = await r.json();
        if (d.ok) {
          showMsg('✓ Enregistré — Reboot pour appliquer', true);
          loadCurrent();
        } else {
          showMsg('✗ Erreur : ' + (d.err || 'inconnue'), false);
        }
      } catch(e) {
        showMsg('✗ Erreur réseau', false);
      }
    }

    async function resetDefaults() {
      if (!confirm('Réinitialiser aux valeurs par défaut ?')) return;
      try {
        const r = await fetch('/api/mqtt/reset', { method: 'POST' });
        const d = await r.json();
        if (d.ok) {
          showMsg('✓ Réinitialisé — Reboot pour appliquer', true);
          loadCurrent();
        }
      } catch(e) { showMsg('✗ Erreur', false); }
    }

    function showMsg(text, ok) {
      const m = document.getElementById('msg');
      m.className = 'msg ' + (ok ? 'ok' : 'err');
      m.textContent = text;
      setTimeout(() => { m.className = 'msg'; m.textContent = ''; }, 5000);
    }

    loadCurrent();
  </script>
</body>
</html>
)rawliteral";
static const char REBOOT_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Reboot — HEELPMEE</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { background: #0d0d0d; color: #e8e8e8; font-family: 'Courier New', monospace;
           padding: 1.5rem; max-width: 600px; margin: 0 auto; }
    h1 { color: #ff4455; letter-spacing: 0.2em; font-size: 1.3rem; text-align: center;
         text-shadow: 0 0 12px #ff445566; margin-bottom: 1rem; }
    .back { color: #888; font-size: 0.8rem; text-decoration: none; letter-spacing: 0.1em; }
    .back:hover { color: #4488ff; }
    .card { background: #1a1a1a; border: 1px solid #333; border-radius: 6px;
            padding: 1.5rem; margin-bottom: 1rem; text-align: center; }
    .icon { font-size: 3rem; margin-bottom: 1rem; }
    .desc { color: #aaa; font-size: 0.9rem; margin-bottom: 1.5rem; line-height: 1.5; }
    button { background: transparent; padding: 0.8rem 1.5rem; font-family: inherit;
             font-size: 0.9rem; letter-spacing: 0.2em; text-transform: uppercase;
             border-radius: 4px; cursor: pointer; transition: all 0.2s; width: 100%;
             border: 1px solid #ff4455; color: #ff4455; }
    button:hover { background: #ff445522; box-shadow: 0 0 12px #ff445544; }
    button:disabled { opacity: 0.4; cursor: default; }
    .warning { background: #ffaa0011; border: 1px solid #ffaa0044; padding: 0.7rem;
               border-radius: 4px; color: #ffaa00; font-size: 0.8rem; margin-top: 1rem;
               line-height: 1.4; text-align: left; }
    .countdown { display: none; margin-top: 1.5rem; text-align: center;
                 color: #4488ff; font-size: 1.5rem; letter-spacing: 0.2em; }
    .countdown.active { display: block; animation: pulse 1s infinite; }
    @keyframes pulse { 50% { opacity: 0.4; } }
    .info { color: #888; font-size: 0.8rem; margin-top: 1rem; text-align: center;
            line-height: 1.5; }
  </style>
</head>
<body>
  <h1>REBOOT DEVICE</h1>
  <a class="back" href="/">← Retour</a>

  <div class="card">
    <div class="icon">⚠️</div>
    <div class="desc">
      Redémarrer le device va interrompre toutes les opérations en cours
      (alertes, audio, MQTT) et appliquer les changements de configuration.
      Le device sera indisponible pendant environ 30 secondes.
    </div>
    <button id="rebootBtn" onclick="doReboot()">Redémarrer maintenant</button>

    <div class="countdown" id="cd">Redémarrage…</div>

    <div class="warning">
      ⚠ Cette page sera inaccessible après le reboot.
      Le WiFi "HEELPMEE-Config" disparaîtra. Pour y revenir, fais un triple
      clic sur le bouton du device.
    </div>
  </div>

  <script>
    async function doReboot() {
      if (!confirm('Redémarrer le device maintenant ?\n\nLes opérations en cours seront interrompues.')) {
        return;
      }

      const btn = document.getElementById('rebootBtn');
      const cd  = document.getElementById('cd');
      btn.disabled = true;
      cd.className = 'countdown active';
      cd.textContent = 'Reboot dans 2s…';

      try {
        await fetch('/api/reboot', { method: 'POST' });
      } catch(e) {
        // Erreur attendue : le device s'éteint, la réponse ne nous parvient pas toujours
      }

      // Compte à rebours visuel
      let seconds = 2;
      const interval = setInterval(() => {
        seconds--;
        if (seconds > 0) {
          cd.textContent = 'Reboot dans ' + seconds + 's…';
        } else {
          cd.textContent = 'Device en redémarrage';
          clearInterval(interval);
        }
      }, 1000);
    }
  </script>
</body>
</html>
)rawliteral";

// ============================================================================
//  HELPERS — récupération des données dynamiques
// ============================================================================

// Retourne le nom de l'état FSM courant
static const char *getFsmName()
{
  DeviceState s = stateMachine_getState();
  if (s == STATE_ACTION)
    return "ACTION";
  if (s == STATE_STANDBY)
    return "STANDBY";
  if (s == STATE_OFF)
    return "OFF";
  return "UNKNOWN";
}

// Récupère le RSSI via AT+CSQ (sans bloquer trop)
static int getLteRssi()
{
  String resp = SentMessageResponse("AT+CSQ", 1500);
  int idx = resp.indexOf("+CSQ: ");
  if (idx == -1)
    return -999;
  int comma = resp.indexOf(",", idx);
  int rssi = resp.substring(idx + 6, comma).toInt();
  if (rssi == 99)
    return -999; // pas de signal
  // Conversion CSQ → dBm : dBm = -113 + 2*rssi
  return -113 + 2 * rssi;
}

// Récupère l'IP LTE via AT+CGPADDR
static String getLteIp()
{
  String resp = SentMessageResponse("AT+CGPADDR=1", 1500);
  int idx = resp.indexOf("+CGPADDR: 1,");
  if (idx == -1)
    return "";
  int s = idx + 12;
  int e = resp.indexOf("\n", s);
  if (e == -1)
    e = resp.length();
  String ip = resp.substring(s, e);
  ip.trim();
  return (ip.length() && ip != "0.0.0.0") ? ip : "";
}

// Compte les fichiers /alerts/alert_*.wav sur SD
static int countAudioFiles()
{
  if (!SD.cardSize())
    return 0;
  File dir = SD.open(AUDIO_DIR_SD);
  if (!dir || !dir.isDirectory())
    return 0;
  int count = 0;
  File entry = dir.openNextFile();
  while (entry)
  {
    if (!entry.isDirectory())
    {
      String name = entry.name();
      if (name.endsWith(".wav"))
        count++;
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  return count;
}

// ============================================================================
//  HANDLERS HTTP
// ============================================================================

static void handleRoot()
{
  captivePortal_kickWatchdog();
  server.send(200, "text/html", INDEX_HTML);
}

// API JSON : renvoie toutes les données dynamiques de l'état
static void handleApiState()
{
  captivePortal_kickWatchdog();

  int rssi = getLteRssi();
  String ip = getLteIp();
  uint32_t shutdownInS = (CAPTIVE_TIMEOUT_MS - (millis() - lastActivityMs)) / 1000;
  if ((millis() - lastActivityMs) > CAPTIVE_TIMEOUT_MS)
    shutdownInS = 0;

  String json = "{";
  json += "\"fsm\":\"" + String(getFsmName()) + "\",";
  json += "\"battery\":" + String(100) + ","; // TODO : vraie lecture batterie
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"transport\":\"" + String(mqttTransport_typeName()) + "\",";
  json += "\"mqtt_connected\":" + String(mqttTransport_isConnected() ? "true" : "false") + ",";
  json += "\"uptime_s\":" + String(millis() / 1000) + ",";
  json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  json += "\"firmware\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"pending_alerts\":" + String(storage_count()) + ",";
  json += "\"audio_count\":" + String(countAudioFiles()) + ",";
  json += "\"shutdown_in_s\":" + String(shutdownInS);
  json += "}";

  server.send(200, "application/json", json);
}

static void handleNotFound()
{
  captivePortal_kickWatchdog();
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}
// ============================================================================
//  HANDLERS WIFI MANAGER
// ============================================================================

static void handleWifiPage()
{
  captivePortal_kickWatchdog();
  server.send(200, "text/html", WIFI_HTML);
}

// API : récupérer les credentials actuels (SSID seulement, pas le password en clair)
static void handleApiWifiGet()
{
  captivePortal_kickWatchdog();
  WifiCreds c = wifiCreds_get();
  String json = "{\"ssid\":\"" + c.ssid + "\"}";
  server.send(200, "application/json", json);
}

// API : scanner les WiFi à portée
static void handleApiWifiScan()
{
  captivePortal_kickWatchdog();
  Serial.println("[PORTAL] Scanning WiFi networks...");

  int n = WiFi.scanNetworks(false, false); // sync, sans cachés

  String json = "{\"networks\":[";
  for (int i = 0; i < n; i++)
  {
    if (i > 0)
      json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\""); // échapper les guillemets
    json += "{\"ssid\":\"" + ssid + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
    json += "}";
  }
  json += "]}";

  WiFi.scanDelete();
  Serial.printf("[PORTAL] Found %d networks\n", n);
  server.send(200, "application/json", json);
}

// API : enregistrer de nouveaux credentials
static void handleApiWifiSet()
{
  captivePortal_kickWatchdog();

  if (!server.hasArg("ssid"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing ssid\"}");
    return;
  }

  String ssid = server.arg("ssid");
  String pwd = server.hasArg("pwd") ? server.arg("pwd") : "";

  bool ok = wifiCreds_set(ssid, pwd);
  String json = "{\"ok\":" + String(ok ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// API : effacer les credentials
static void handleApiWifiClear()
{
  captivePortal_kickWatchdog();
  wifiCreds_clear();
  server.send(200, "application/json", "{\"ok\":true}");
}
void captivePortal_stop()
{
  if (!portalActive)
    return;

  Serial.println("[PORTAL] Stopping captive portal...");
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  portalActive = false;
  Serial.println("[PORTAL] Stopped, WiFi off");
}
// ============================================================================
//  HANDLERS AUDIOS SD
// ============================================================================

static void handleAudiosPage()
{
  captivePortal_kickWatchdog();
  server.send(200, "text/html", AUDIOS_HTML);
}

// API : liste les fichiers WAV + statistiques de stockage
static void handleApiAudiosList()
{
    captivePortal_kickWatchdog();

    // Remonter la SD avec le même objet SPI global pour invalider le cache
    Serial.println("[PORTAL] Remounting SD for listing...");
    SD.end();
    delay(100);
    extern SPIClass sdSpi;
    sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, sdSpi))
    {
        Serial.println("[PORTAL] SD remount FAILED");
        server.send(500, "application/json", "{\"err\":\"SD remount failed\"}");
        return;
    }
    Serial.println("[PORTAL] SD remounted OK");

    if (!SD.cardSize())
    {
        server.send(500, "application/json", "{\"err\":\"SD not available\"}");
        return;
    }

    // Construire la liste JSON
    String json = "{\"count\":0,\"used\":0,\"free\":0,\"files\":[";

    File dir = SD.open(AUDIO_DIR_SD);
    if (!dir || !dir.isDirectory())
    {
        json += "]}";
        server.send(200, "application/json", json);
        return;
    }

    int count = 0;
    uint64_t totalSize = 0;
    bool first = true;
    File entry = dir.openNextFile();
    while (entry)
    {
        if (!entry.isDirectory())
        {
            String name = entry.name();
            if (name.endsWith(".wav"))
            {
                uint32_t size = entry.size();
                totalSize += size;
                count++;
                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"" + name + "\",\"size\":" + String(size) + "}";
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    json += "]";

    uint64_t cardSize = SD.cardSize();
    uint64_t cardUsed = SD.usedBytes();
    uint64_t cardFree = cardSize - cardUsed;
    json.replace("\"count\":0", "\"count\":" + String(count));
    json.replace("\"used\":0",  "\"used\":"  + String((uint32_t)cardUsed));
    json.replace("\"free\":0",  "\"free\":"  + String((uint32_t)cardFree));

    json += "}";
    server.send(200, "application/json", json);
}
// API : télécharger un fichier audio
static void handleApiAudiosDownload()
{
  captivePortal_kickWatchdog();

  if (!server.hasArg("f"))
  {
    server.send(400, "text/plain", "Missing filename");
    return;
  }

  String name = server.arg("f");

  // Sécurité : refuser les '/' et '..' pour éviter le path traversal
  if (name.indexOf('/') != -1 || name.indexOf("..") != -1)
  {
    server.send(400, "text/plain", "Invalid filename");
    return;
  }

  String path = String(AUDIO_DIR_SD) + "/" + name;

  if (!SD.exists(path))
  {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f)
  {
    server.send(500, "text/plain", "Cannot open file");
    return;
  }

  Serial.printf("[PORTAL] Downloading %s (%u bytes)\n", path.c_str(), f.size());

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.sendHeader("Content-Length", String(f.size()));
  server.streamFile(f, "audio/wav");
  f.close();
  Serial.println("[PORTAL] Download done");
}

// API : supprimer un fichier audio individuel
static void handleApiAudiosDelete()
{
  captivePortal_kickWatchdog();

  if (!server.hasArg("f"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing\"}");
    return;
  }

  String name = server.arg("f");
  if (name.indexOf('/') != -1 || name.indexOf("..") != -1)
  {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"invalid\"}");
    return;
  }

  String path = String(AUDIO_DIR_SD) + "/" + name;

  if (!SD.exists(path))
  {
    server.send(404, "application/json", "{\"ok\":false,\"err\":\"not found\"}");
    return;
  }

  bool ok = SD.remove(path);
  Serial.printf("[PORTAL] Delete %s : %s\n", path.c_str(), ok ? "OK" : "FAILED");

  String json = "{\"ok\":" + String(ok ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// API : effacer tous les fichiers audio
static void handleApiAudiosClear()
{
  captivePortal_kickWatchdog();

  if (!SD.cardSize())
  {
    server.send(500, "application/json", "{\"ok\":false,\"err\":\"SD not available\"}");
    return;
  }

  File dir = SD.open(AUDIO_DIR_SD);
  if (!dir || !dir.isDirectory())
  {
    server.send(200, "application/json", "{\"ok\":true,\"deleted\":0}");
    return;
  }

  // Construire d'abord la liste, puis supprimer (sinon iter cassé)
  std::vector<String> toDelete;
  File entry = dir.openNextFile();
  while (entry)
  {
    if (!entry.isDirectory())
    {
      String name = entry.name();
      if (name.endsWith(".wav"))
      {
        toDelete.push_back(String(AUDIO_DIR_SD) + "/" + name);
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  int deleted = 0;
  for (auto &path : toDelete)
  {
    if (SD.remove(path))
      deleted++;
  }

  Serial.printf("[PORTAL] Cleared %d audio files\n", deleted);
  String json = "{\"ok\":true,\"deleted\":" + String(deleted) + "}";
  server.send(200, "application/json", json);
}
// ============================================================================
//  HANDLERS CONFIG MQTT
// ============================================================================

static void handleMqttPage()
{
  captivePortal_kickWatchdog();
  server.send(200, "text/html", MQTT_HTML);
}

// API : récupérer la config MQTT active
static void handleApiMqttGet()
{
  captivePortal_kickWatchdog();
  MqttConfig c = mqttConfig_get();

  String json = "{";
  json += "\"broker\":\"" + c.broker + "\",";
  json += "\"port\":" + String(c.port) + ",";
  json += "\"clientId\":\"" + c.clientId + "\",";
  json += "\"custom\":" + String(mqttConfig_isCustom() ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// API : sauvegarder une nouvelle config MQTT
static void handleApiMqttSet()
{
  captivePortal_kickWatchdog();

  if (!server.hasArg("broker") || !server.hasArg("port") || !server.hasArg("clientid"))
  {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"missing fields\"}");
    return;
  }

  String broker = server.arg("broker");
  String portStr = server.arg("port");
  String clientId = server.arg("clientid");

  // Validation basique
  broker.trim();
  portStr.trim();
  clientId.trim();

  if (broker.length() == 0 || clientId.length() == 0)
  {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"empty values\"}");
    return;
  }

  int portNum = portStr.toInt();
  if (portNum < 1 || portNum > 65535)
  {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"invalid port\"}");
    return;
  }

  bool ok = mqttConfig_set(broker, (uint16_t)portNum, clientId);
  String json = "{\"ok\":" + String(ok ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// API : reset aux defaults
static void handleApiMqttReset()
{
  captivePortal_kickWatchdog();
  bool ok = mqttConfig_reset();
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + "}");
}
// ============================================================================
//  HANDLERS REBOOT
// ============================================================================

static void handleRebootPage()
{
  captivePortal_kickWatchdog();
  server.send(200, "text/html", REBOOT_HTML);
}

// API : déclenche le reboot après un court délai (laisse le temps à la réponse HTTP de partir)
static void handleApiReboot()
{
  captivePortal_kickWatchdog();

  Serial.println("[PORTAL] Reboot requested by user");

  // Répondre AVANT de rebooter, sinon le client attend indéfiniment
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting in 2s\"}");
  server.client().flush(); // forcer l'envoi avant le reboot

  // Petit délai pour que la réponse parte et que le portail se ferme proprement
  delay(2000);

  // Arrêt propre du portail (libère AP + serveur HTTP)
  captivePortal_stop();
  delay(100);

  Serial.println("[PORTAL] *** REBOOTING NOW ***");
  Serial.flush(); // s'assurer que les logs partent avant le reset

  ESP.restart();
}
// ============================================================================
//  HANDLER LTE
// ============================================================================
static void handleApiForceLte()
{
  captivePortal_kickWatchdog();
  Serial.println("[PORTAL] User requested LTE mode (resetting force_wifi flag)");
  bootMode_setForceWifi(false);

  server.send(200, "application/json", "{\"ok\":true}");
  server.client().flush();
  delay(1500);

  captivePortal_stop();
  delay(100);
  ESP.restart();
}

// ============================================================================
//  START / STOP
// ============================================================================
bool captivePortal_start()
{
  if (portalActive)
  {
    Serial.println("[PORTAL] Already active");
    return true;
  }

  Serial.println("[PORTAL] Starting captive portal...");

  WiFi.mode(WIFI_AP_STA); // mode AP+STA pour pouvoir se connecter à un WiFi et garder l'AP actif
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));

  bool ok = WiFi.softAP(CAPTIVE_AP_SSID,
                        strlen(CAPTIVE_AP_PASSWORD) >= 8 ? CAPTIVE_AP_PASSWORD : NULL,
                        CAPTIVE_AP_CHANNEL,
                        0,
                        CAPTIVE_AP_MAX_CLIENTS);

  if (!ok)
  {
    Serial.println("[PORTAL] ERROR: softAP failed");
    return false;
  }

  Serial.printf("[PORTAL] AP SSID: %s\n", CAPTIVE_AP_SSID);
  Serial.printf("[PORTAL] AP IP:   %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", AP_IP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleApiState);

  // WiFi Manager
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/api/wifi/get", HTTP_GET, handleApiWifiGet);
  server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);
  server.on("/api/wifi/set", HTTP_POST, handleApiWifiSet);
  server.on("/api/wifi/clear", HTTP_POST, handleApiWifiClear);
  // Audios SD
  server.on("/audios", HTTP_GET, handleAudiosPage);
  server.on("/api/audios/list", HTTP_GET, handleApiAudiosList);
  server.on("/api/audios/download", HTTP_GET, handleApiAudiosDownload);
  server.on("/api/audios/delete", HTTP_POST, handleApiAudiosDelete);
  server.on("/api/audios/clear", HTTP_POST, handleApiAudiosClear);
  // MQTT Config

  server.on("/mqtt", HTTP_GET, handleMqttPage);
  server.on("/api/mqtt/get", HTTP_GET, handleApiMqttGet);
  server.on("/api/mqtt/set", HTTP_POST, handleApiMqttSet);
  server.on("/api/mqtt/reset", HTTP_POST, handleApiMqttReset);
  // Reboot
  server.on("/reboot", HTTP_GET, handleRebootPage);
  server.on("/api/reboot", HTTP_POST, handleApiReboot);
  // Force LTE
  server.on("/api/wifi/force_lte", HTTP_POST, handleApiForceLte);

  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[PORTAL] HTTP server started on port 80");
  Serial.println("[PORTAL] Connect to 'HEELPMEE-Config' and open http://192.168.4.1/");

  portalActive = true;
  lastActivityMs = millis();
  return true;
}

void captivePortal_loop()
{
  if (!portalActive)
    return;

  dnsServer.processNextRequest();
  server.handleClient();

  if (millis() - lastActivityMs > CAPTIVE_TIMEOUT_MS)
  {
    Serial.printf("[PORTAL] Inactivity timeout (%dms) — auto-shutdown\n",
                  CAPTIVE_TIMEOUT_MS);
    captivePortal_stop();
  }
}

bool captivePortal_isActive()
{
  return portalActive;
}

void captivePortal_kickWatchdog()
{
  lastActivityMs = millis();
}