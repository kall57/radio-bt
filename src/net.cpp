#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "storage.h"

namespace {
DNSServer dns;
bool ap = false;
bool mdnsUp = false;
String staSsid, staPass;
uint32_t lastAttempt = 0, lastCheck = 0;
volatile int lastReason = 0;  // dernière raison de déconnexion Wi-Fi (code ESP-IDF)

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    int r = info.wifi_sta_disconnected.reason;
    if (r != lastReason) {  // évite de répéter la même ligne à chaque essai
      Serial.printf("\n[wifi] échec / déconnexion : raison %d (%s)\n", r,
                    WiFi.STA.disconnectReasonName((wifi_err_reason_t)r));
    }
    lastReason = r;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    lastReason = 0;
  }
}

// Lance une tentative de connexion propre (arrête d'abord celle en cours,
// sinon : "sta is connecting, cannot set config")
void connectSta() {
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
}

void startMdns() {
  if (mdnsUp) return;
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
  }
}

void startAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  delay(100);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  ap = true;
  Serial.printf("[wifi] point d'accès \"%s\" : http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopAp() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
  Serial.println("[wifi] réseau rejoint, point d'accès arrêté");
}
}  // namespace

namespace Net {

void begin() {
  staSsid = Storage::getString("wifi_ssid", WIFI_DEFAULT_SSID);
  staPass = Storage::getString("wifi_pass", WIFI_DEFAULT_PASS);

  WiFi.persistent(false);
  WiFi.onEvent(onWifiEvent);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Cherche sur tous les canaux et choisit la borne qui capte le mieux
  // (utile avec un répéteur ou un réseau maillé portant le même nom)
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  if (staSsid.length()) {
    // 3 tentatives : la première échoue parfois juste après la mise sous tension
    for (int attempt = 1; attempt <= 3 && WiFi.status() != WL_CONNECTED; attempt++) {
      Serial.printf("[wifi] connexion à \"%s\" (essai %d/3)", staSsid.c_str(), attempt);
      connectSta();
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS / 2) {
        delay(250);
        Serial.print('.');
      }
      Serial.println();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connecté : http://%s  ou  http://%s.local\n", WiFi.localIP().toString().c_str(),
                  DEVICE_HOSTNAME);
    startMdns();
  } else {
    startAp();
  }
  lastAttempt = millis();
}

void loop() {
  if (ap) {
    dns.processNextRequest();
    if (WiFi.status() == WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
      stopAp();
      startMdns();
    } else if (staSsid.length() && WiFi.status() != WL_CONNECTED &&
               millis() - lastAttempt > AP_RETRY_STA_MS && WiFi.softAPgetStationNum() == 0) {
      lastAttempt = millis();
      Serial.printf("[wifi] nouvel essai de connexion à \"%s\"\n", staSsid.c_str());
      connectSta();  // le routeur est peut-être revenu
    }
    return;
  }
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }
}

bool apMode() { return ap; }
bool connected() { return WiFi.status() == WL_CONNECTED; }
String ssid() { return staSsid; }
String ip() { return connected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
int rssi() { return connected() ? WiFi.RSSI() : 0; }
String lastError() {
  int r = lastReason;
  return r ? String(WiFi.STA.disconnectReasonName((wifi_err_reason_t)r)) + " (" + r + ")" : String();
}

int scan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true);
    return -1;
  }
  return n;  // WIFI_SCAN_RUNNING (-1) ou nombre de réseaux
}
String scanSsid(int i) { return WiFi.SSID(i); }
int scanRssi(int i) { return WiFi.RSSI(i); }
bool scanSecure(int i) { return WiFi.encryptionType(i) != WIFI_AUTH_OPEN; }
void scanClear() { WiFi.scanDelete(); }

void saveCredentials(const String& s, const String& p) {
  Storage::putString("wifi_ssid", s);
  Storage::putString("wifi_pass", p);
}

}  // namespace Net
