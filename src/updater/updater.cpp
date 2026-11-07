/*
  Radio BT — firmware de MISE À JOUR (environnement PlatformIO "updater")

  Le firmware principal (~2,1 Mo) est trop gros pour la mise à jour Wi-Fi
  classique, qui demande deux emplacements de sa taille. On utilise donc
  ce petit firmware séparé, installé une fois pour toutes dans "ota_1" :

    appli (ota_0) --[Réglages > Mettre à jour]--> redémarre sur ota_1
    ota_1 (ici)   --[envoi de firmware.bin]-----> écrit ota_0, redémarre dessus

  Il reprend le Wi-Fi enregistré par l'appli (même espace NVS "radiobt").
  Sans action pendant 15 min, il revient automatiquement sur l'appli.
*/
#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "../config.h"

#ifndef UPDATER_VERSION
#define UPDATER_VERSION "1.0.0"
#endif

// Repère cherché par la radio pour savoir si ce module est installé (voir ota.cpp)
extern "C" const char RADIOBT_UPDATER_MAGIC[] __attribute__((used)) = "RADIOBT-UPDATER-V1";

static WebServer server(80);
static DNSServer dns;
static bool apMode = false;
static uint32_t lastActivity = 0;
static uint32_t restartAt = 0;
static String uploadError;
static size_t uploadTotal = 0;

static const uint32_t IDLE_TIMEOUT_MS = 15UL * 60UL * 1000UL;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Radio BT — mise à jour</title>
<style>
:root{--bg:#E7E8E6;--surface:#F6F6F4;--ink:#1F2328;--mute:#62686D;--line:#C9CDCC;--needle:#D6372B;--ok:#2F7D55}
@media (prefers-color-scheme:dark){:root{--bg:#181B1D;--surface:#212528;--ink:#E5E8E9;--mute:#979EA3;--line:#353B3F;--needle:#F0533F;--ok:#55B784}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 "Avenir Next","Segoe UI",Roboto,Arial,sans-serif}
.wrap{max-width:560px;margin:0 auto;padding:28px 16px 48px}
h1{font-size:20px;margin:0 0 6px}p{color:var(--mute);margin:0 0 16px}
.panel{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:18px}
input[type=file]{width:100%;padding:10px;border:1px dashed var(--line);border-radius:8px;background:var(--bg);margin-bottom:14px}
button{font:inherit;padding:9px 15px;border-radius:7px;border:1px solid var(--line);background:var(--surface);color:var(--ink);cursor:pointer;font-weight:500}
button.primary{background:var(--ink);color:var(--bg);border-color:var(--ink)}button:disabled{opacity:.5;cursor:default}
.bar{display:flex;gap:8px;flex-wrap:wrap}
progress{width:100%;height:10px;margin:16px 0 6px;accent-color:var(--ink)}
#msg{min-height:1.5em;margin:6px 0 0}#msg.err{color:var(--needle)}#msg.ok{color:var(--ok)}
code{font-size:13px}
</style></head><body><div class="wrap">
<h1>Mise à jour du firmware</h1>
<p>Choisissez le fichier <code>firmware.bin</code> produit par PlatformIO
(<code>.pio/build/esp32cam/firmware.bin</code>). La radio reprend automatiquement après l'installation.</p>
<div class="panel">
  <input type="file" id="f" accept=".bin">
  <div class="bar">
    <button class="primary" id="go">Installer</button>
    <button id="cancel">Revenir à la radio sans mettre à jour</button>
  </div>
  <progress id="p" max="100" value="0" hidden></progress>
  <div id="msg"></div>
</div>
<p style="margin-top:16px;font-size:13px">Mode mise à jour %VER% — retour automatique à la radio après 15 min sans action.</p>
</div>
<script>
const $=id=>document.getElementById(id),msg=(t,c)=>{$("msg").textContent=t;$("msg").className=c||""};
function waitBack(){setTimeout(function poll(){fetch("/api/status",{cache:"no-store"}).then(r=>r.json())
 .then(s=>{if(s.mode!=="updater")location.href="/";else setTimeout(poll,1500)}).catch(()=>setTimeout(poll,1500))},4000)}
$("go").onclick=()=>{
 const f=$("f").files[0];
 if(!f)return msg("Choisissez d'abord le fichier firmware.bin.","err");
 if(!/\.bin$/i.test(f.name))return msg("Le fichier doit se terminer par .bin.","err");
 $("go").disabled=$("cancel").disabled=true;$("p").hidden=false;msg("Envoi en cours…");
 const x=new XMLHttpRequest(),d=new FormData();d.append("firmware",f,f.name);
 x.upload.onprogress=e=>{if(e.lengthComputable){$("p").value=e.loaded*100/e.total;msg("Envoi : "+Math.round(e.loaded*100/e.total)+" %")}};
 x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(e){}
  if(x.status===200&&r.ok){$("p").value=100;msg("Mise à jour installée. Redémarrage de la radio…","ok");waitBack()}
  else{msg(r.error||("Échec ("+x.status+")"),"err");$("go").disabled=$("cancel").disabled=false}};
 x.onerror=()=>{msg("Connexion perdue pendant l'envoi. Réessayez.","err");$("go").disabled=$("cancel").disabled=false};
 x.open("POST","/update");x.send(d)};
$("cancel").onclick=()=>{$("go").disabled=$("cancel").disabled=true;
 fetch("/cancel",{method:"POST"}).then(()=>{msg("Retour à la radio…","ok");waitBack()})};
</script></body></html>)HTML";

static bool bootApp() {
  const esp_partition_t* app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  return app && esp_ota_set_boot_partition(app) == ESP_OK;
}

static void handleUpload() {
  HTTPUpload& up = server.upload();
  lastActivity = millis();
  if (up.status == UPLOAD_FILE_START) {
    uploadError = "";
    uploadTotal = 0;
    Serial.printf("[maj] réception de %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) uploadError = String("Préparation impossible : ") + Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadTotal == 0 && up.currentSize > 0 && up.buf[0] != 0xE9) {
      uploadError = "Ce fichier n'est pas un firmware ESP32 (firmware.bin attendu)";
      Update.abort();
    }
    if (uploadError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize)
      uploadError = String("Écriture impossible : ") + Update.errorString();
    uploadTotal += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadError.isEmpty() && !Update.end(true))  // true : définit l'appli comme partition de démarrage
      uploadError = String("Firmware refusé : ") + Update.errorString();
    Serial.printf("[maj] %u octets reçus — %s\n", (unsigned)uploadTotal, uploadError.isEmpty() ? "OK" : uploadError.c_str());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    uploadError = "Envoi interrompu";
  }
}

static void handleUpdateDone() {
  if (!uploadError.isEmpty() || !Update.isFinished()) {
    String e = uploadError.isEmpty() ? String("Envoi incomplet") : uploadError;
    e.replace("\"", "'");
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"" + e + "\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  restartAt = millis() + 1000;
}

static void connectWifi() {
  Preferences prefs;
  prefs.begin("radiobt", true);
  String ssid = prefs.isKey("wifi_ssid") ? prefs.getString("wifi_ssid") : String(WIFI_DEFAULT_SSID);
  String pass = prefs.isKey("wifi_pass") ? prefs.getString("wifi_pass") : String(WIFI_DEFAULT_PASS);
  prefs.end();

  WiFi.persistent(false);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  for (int i = 0; i < 3 && ssid.length() && WiFi.status() != WL_CONNECTED; i++) {
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());
    for (uint32_t t0 = millis(); WiFi.status() != WL_CONNECTED && millis() - t0 < 10000;) delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[maj] http://%s  ou  http://%s.local\n", WiFi.localIP().toString().c_str(), DEVICE_HOSTNAME);
    if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    dns.start(53, "*", WiFi.softAPIP());
    apMode = true;
    Serial.printf("[maj] Wi-Fi indisponible : point d'accès \"%s\", http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  Serial.printf("\nRadio BT — mode mise à jour %s (%s)\n", UPDATER_VERSION, RADIOBT_UPDATER_MAGIC);

  connectWifi();

  server.on("/", HTTP_GET, []() {
    String page = FPSTR(PAGE);
    page.replace("%VER%", UPDATER_VERSION);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", page);
  });
  server.on("/api/status", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"mode\":\"updater\",\"fw\":\"" UPDATER_VERSION "\"}");
  });
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);
  server.on("/cancel", HTTP_POST, []() {
    bool ok = bootApp();
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    if (ok) restartAt = millis() + 800;
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);  // portail captif / ancienne adresse de l'appli
    server.send(302, "text/plain", "");
  });
  server.begin();
  lastActivity = millis();
}

void loop() {
  server.handleClient();
  if (apMode) dns.processNextRequest();
  digitalWrite(STATUS_LED_PIN, (millis() / 400) % 2 ? LOW : HIGH);  // clignotement moyen = mode mise à jour

  if (restartAt && millis() > restartAt) ESP.restart();
  if (!restartAt && millis() - lastActivity > IDLE_TIMEOUT_MS) {
    Serial.println("[maj] aucune action : retour à la radio");
    if (bootApp()) ESP.restart();
    lastActivity = millis();
  }
  delay(2);
}
