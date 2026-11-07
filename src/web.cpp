#include "web.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "bt.h"
#include "config.h"
#include "live.h"
#include "ota.h"
#include "net.h"
#include "player.h"
#include "storage.h"
#include "web_index.h"

namespace {
WebServer server(80);
uint32_t restartAt = 0;

void sendJson(JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }

void sendError(int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(d, code);
}

bool readBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) return false;
  return !deserializeJson(doc, server.arg("plain"));
}

void scheduleRestart() { restartAt = millis() + 800; }

bool validUrl(const String& u) {
  return u.startsWith("http://") || u.startsWith("https://") || u.startsWith("pcm://");  // pcm:// = SDR++
}

// ---------------------------------------------------------------------------
void handleIndex() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
}

void handleStatus() {
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["heapMin"] = ESP.getMinFreeHeap();
  d["heapBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  d["psram"] = ESP.getFreePsram();

  JsonObject w = d["wifi"].to<JsonObject>();
  w["mode"] = Net::apMode() ? "ap" : "sta";
  w["connected"] = Net::connected();
  w["ssid"] = Net::ssid();
  w["ip"] = Net::ip();
  w["rssi"] = Net::rssi();
  w["error"] = Net::lastError();
  w["host"] = DEVICE_HOSTNAME;
  w["ap"] = AP_SSID;

  JsonObject b = d["bt"].to<JsonObject>();
  b["state"] = Bt::state();
  b["name"] = Bt::targetName();
  b["mac"] = Bt::targetMac();
  b["discovering"] = Bt::discovering();
  JsonArray devs = b["devices"].to<JsonArray>();
  for (const auto& dev : Bt::devices()) {
    JsonObject o = devs.add<JsonObject>();
    o["name"] = dev.name;
    o["mac"] = dev.mac;
    o["rssi"] = dev.rssi;
    o["age"] = (millis() - dev.lastSeen) / 1000;
  }

  Player::Info i = Player::info();
  JsonObject p = d["player"].to<JsonObject>();
  p["state"] = i.state;
  p["wanted"] = i.wanted;
  p["name"] = i.name;
  p["url"] = i.url;
  p["title"] = i.title;
  p["codec"] = i.codec;
  p["error"] = i.error;
  p["rate"] = i.sampleRate;
  p["channels"] = i.channels;
  p["bitrate"] = i.bitrate;
  p["buffer"] = i.bufferPct;
  p["underruns"] = i.underruns;
  p["elapsed"] = i.elapsedS;
  p["volume"] = i.volume;
  p["netKbps"] = i.netKbps;
  p["live"] = i.live;

  JsonObject lv = d["live"].to<JsonObject>();
  lv["enabled"] = Live::enabled();
  lv["port"] = Live::port();
  lv["password"] = Live::password();
  lv["listeners"] = Live::listenerCount();
  lv["maxListeners"] = LIVE_MAX_LISTENERS;

  JsonObject ot = d["ota"].to<JsonObject>();
  ot["partitions"] = Ota::partitionsReady();
  ot["updater"] = Ota::updaterInstalled();
  p["autoplay"] = Storage::getBool("autoplay", true);
  sendJson(d);
}

void handleGetStations() {
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (const auto& s : Storage::stations()) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    o["url"] = s.url;
    o["fav"] = s.fav;
  }
  sendJson(d);
}

void handlePutStations() {
  JsonDocument d;
  if (!readBody(d) || !d.is<JsonArray>()) return sendError(400, "Liste de stations invalide");
  std::vector<Station> list;
  for (JsonObject o : d.as<JsonArray>()) {
    String name = o["name"] | "";
    String url = o["url"] | "";
    name.trim();
    url.trim();
    if (!validUrl(url)) return sendError(400, "Chaque station doit avoir une adresse http://, https:// ou pcm:// (SDR++)");
    if (name.isEmpty()) name = url;
    list.push_back({name.substring(0, 60), url.substring(0, 300), o["fav"] | false});
    if (list.size() > MAX_STATIONS) return sendError(400, "Trop de stations (40 au maximum)");
  }
  if (!Storage::saveStations(list)) return sendError(500, "Enregistrement impossible");
  handleGetStations();
}

void handleResetStations() {
  Storage::resetStations();
  handleGetStations();
}

void handlePlay() {
  JsonDocument d;
  if (!readBody(d)) return sendError(400, "Requête invalide");
  String url, name;
  if (d["index"].is<int>()) {
    int i = d["index"];
    const auto& list = Storage::stations();
    if (i < 0 || i >= (int)list.size()) return sendError(404, "Station introuvable");
    url = list[i].url;
    name = list[i].name;
  } else {
    url = d["url"] | "";
    name = d["name"] | "";
    url.trim();
    if (!validUrl(url)) return sendError(400, "L'adresse doit commencer par http://, https:// ou pcm:// (SDR++)");
    if (name.isEmpty()) name = "Flux personnalisé";
  }
  Storage::putString("last_url", url);
  Storage::putString("last_name", name);
  Storage::putBool("was_playing", true);
  Player::play(url, name);
  sendOk();
}

void handleStop() {
  Player::stop();
  Storage::putBool("was_playing", false);
  sendOk();
}

void handleVolume() {
  JsonDocument d;
  if (!readBody(d) || !d["volume"].is<int>()) return sendError(400, "Volume manquant");
  Player::setVolume(d["volume"]);
  Storage::putInt("volume", Player::volume());
  sendOk();
}

void handleSettings() {
  JsonDocument d;
  if (!readBody(d)) return sendError(400, "Requête invalide");
  if (d["autoplay"].is<bool>()) Storage::putBool("autoplay", d["autoplay"]);
  // Source directe : on vérifie tout avant d'enregistrer quoi que ce soit
  int port = d["livePort"].is<int>() ? d["livePort"].as<int>() : -1;
  String pw = d["livePassword"].is<const char*>() ? d["livePassword"].as<String>() : String();
  pw.trim();
  if (d["livePort"].is<int>() && (port < 1 || port > 65535 || port == 80))
    return sendError(400, "Port invalide (1 à 65535, sauf 80)");
  if (d["livePassword"].is<const char*>() && (pw.isEmpty() || pw.length() > 40))
    return sendError(400, "Mot de passe vide ou trop long (40 caractères au plus)");
  bool liveChanged = false;
  if (d["liveEnabled"].is<bool>()) {
    Storage::putBool("live_on", d["liveEnabled"]);
    liveChanged = true;
  }
  if (port > 0) {
    Storage::putInt("live_port", port);
    liveChanged = true;
  }
  if (pw.length()) {
    Storage::putString("live_pass", pw);
    liveChanged = true;
  }
  if (liveChanged) Live::restart();
  sendOk();
}

void handleBtSelect() {
  JsonDocument d;
  if (!readBody(d)) return sendError(400, "Requête invalide");
  String name = d["name"] | "";
  String mac = d["mac"] | "";
  if (mac.length() != 17) return sendError(400, "Adresse de l'enceinte invalide");
  Bt::select(name, mac);
  sendOk();
  scheduleRestart();
}

void handleBtForget() {
  Bt::forget();
  sendOk();
  scheduleRestart();
}

void handleWifiScan() {
  int n = Net::scan();
  JsonDocument d;
  d["scanning"] = n < 0;
  JsonArray arr = d["networks"].to<JsonArray>();
  if (n >= 0) {
    for (int i = 0; i < n; i++) {
      String s = Net::scanSsid(i);
      if (s.isEmpty()) continue;
      bool dup = false;
      for (JsonObject o : arr)
        if (s == o["ssid"].as<const char*>()) dup = true;  // même réseau sur plusieurs bornes
      if (dup) continue;
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = s;
      o["rssi"] = Net::scanRssi(i);
      o["secure"] = Net::scanSecure(i);
    }
    Net::scanClear();
  }
  sendJson(d);
}

void handleWifiSave() {
  JsonDocument d;
  if (!readBody(d)) return sendError(400, "Requête invalide");
  String ssid = d["ssid"] | "";
  String pass = d["password"] | "";
  if (ssid.isEmpty()) return sendError(400, "Nom du réseau manquant");
  Net::saveCredentials(ssid, pass);
  sendOk();
  scheduleRestart();
}

// --- Mise à jour par Wi-Fi ---------------------------------------------------
String otaError = "Aucun fichier reçu";
bool otaFailed = true;  // remis à zéro au début de chaque envoi

void handleUpdaterUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaError = "";
    otaFailed = !Ota::updaterUploadStart(otaError);
    Serial.printf("[ota] réception du module : %s\n", up.filename.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaFailed) otaFailed = !Ota::updaterUploadWrite(up.buf, up.currentSize, otaError);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!otaFailed) otaFailed = !Ota::updaterUploadEnd(otaError);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Ota::updaterUploadAbort();
    otaFailed = true;
    otaError = "Envoi interrompu";
  }
}

void handleUpdaterDone() {
  if (otaFailed || otaError.length()) sendError(400, otaError.length() ? otaError.c_str() : "Échec de l'installation");
  else sendOk();
  otaFailed = true;  // une requête sans fichier ne doit pas passer pour un succès
  otaError = "Aucun fichier reçu";
}

void handleOtaStart() {
  if (!Ota::rebootToUpdater()) return sendError(409, "Module de mise à jour absent : installez-le d'abord");
  Player::stop();
  sendOk();
  scheduleRestart();
}

void handleRestart() {
  sendOk();
  scheduleRestart();
}

// Portail captif : en mode point d'accès, toute adresse inconnue renvoie vers l'interface
void handleNotFound() {
  if (server.uri().startsWith("/api/")) return sendError(404, "Ressource inconnue");
  if (Net::apMode()) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Introuvable");
}
}  // namespace

namespace Web {

void begin() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/stations", HTTP_GET, handleGetStations);
  server.on("/api/stations", HTTP_PUT, handlePutStations);
  server.on("/api/stations/reset", HTTP_POST, handleResetStations);
  server.on("/api/play", HTTP_POST, handlePlay);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/volume", HTTP_POST, handleVolume);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/bt/select", HTTP_POST, handleBtSelect);
  server.on("/api/bt/forget", HTTP_POST, handleBtForget);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/ota/updater", HTTP_POST, handleUpdaterDone, handleUpdaterUpload);
  server.on("/api/ota/start", HTTP_POST, handleOtaStart);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();
  if (restartAt && millis() > restartAt) {
    Serial.println("[sys] redémarrage");
    delay(100);
    ESP.restart();
  }
}

}  // namespace Web
