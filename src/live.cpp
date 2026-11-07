#include "live.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#ifndef LIVE_HOST_TEST
#include <lwip/sockets.h>
#endif

#include "bt.h"
#include "player.h"
#include "storage.h"

namespace {
WiFiServer* server = nullptr;
bool on = true;
int listenPort = 8000;
String pass = "radiobt";

const char* kOk = "HTTP/1.0 200 OK\r\nServer: Icecast 2.4.4\r\nConnection: Close\r\n\r\n";

// ---------------------------------------------------------------------------
// Relais vers les navigateurs : les octets MP3/AAC reçus de la source sont
// copiés tels quels dans un tampon circulaire (PSRAM), puis renvoyés aux
// auditeurs. Aucun décodage : seule la radio Wi-Fi est sollicitée.
// Écrit par la tâche audio (relayPush), lu par la boucle principale.
// ---------------------------------------------------------------------------
const size_t RELAY_SIZE = 64 * 1024;     // ~4 s à 128 kbit/s
const size_t RELAY_START = 16 * 1024;    // envoyé d'un coup à un nouvel auditeur (~1 s)
const size_t RELAY_MARGIN = 8 * 1024;    // un auditeur plus en retard que SIZE-MARGIN est recalé
uint8_t* relay = nullptr;
volatile uint32_t relayHead = 0;         // nombre total d'octets écrits (compteur circulaire)
volatile uint32_t relayGen = 0;          // change à chaque début / fin de diffusion
volatile bool relayActive = false;
String relayType = "audio/mpeg";

struct Listener {
  WiFiClient c;
  uint32_t pos = 0;
  uint32_t gen = 0;
  bool used = false;
};
Listener listeners[LIVE_MAX_LISTENERS];

// Envoi non bloquant : un auditeur lent ne doit jamais ralentir la boucle
int sendNow(WiFiClient& c, const uint8_t* p, size_t n) {
#ifdef LIVE_HOST_TEST
  return (int)c.write(p, n);
#else
  int r = ::send(c.fd(), p, n, MSG_DONTWAIT);
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return r;
#endif
}

void dropListener(Listener& l) {
  l.c.stop();
  l.c = WiFiClient();
  l.used = false;
}

void serviceListeners() {
  const uint32_t head = relayHead, gen = relayGen;
  for (auto& l : listeners) {
    if (!l.used) continue;
    if (l.gen != gen || !relayActive || !l.c.connected()) {
      dropListener(l);
      continue;
    }
    for (int burst = 0; burst < 4; burst++) {  // au plus ~6 Ko par auditeur et par tour
      uint32_t avail = head - l.pos;
      if (avail > RELAY_SIZE - RELAY_MARGIN) {  // trop en retard : on le recale sur le présent
        l.pos = head - RELAY_START;
        avail = RELAY_START;
      }
      if (!avail) break;
      size_t off = l.pos % RELAY_SIZE;
      size_t n = min((size_t)avail, min((size_t)1460, RELAY_SIZE - off));
      int sent = sendNow(l.c, relay + off, n);
      if (sent < 0) {
        dropListener(l);
        break;
      }
      if (sent == 0) break;  // tampon d'envoi plein : on réessaiera au prochain tour
      l.pos += sent;
    }
  }
}

void addListener(WiFiClient& c) {
  for (auto& l : listeners)  // libère d'abord les places d'auditeurs d'une diffusion terminée
    if (l.used && (l.gen != relayGen || !l.c.connected())) dropListener(l);
  if (!relay || !relayActive) {
    c.print("HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: Close\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\nAucune diffusion en cours\r\n");
    c.stop();
    return;
  }
  for (auto& l : listeners) {
    if (l.used) continue;
    c.printf("HTTP/1.0 200 OK\r\nServer: Icecast 2.4.4\r\nContent-Type: %s\r\nCache-Control: no-cache, no-store\r\n"
             "Access-Control-Allow-Origin: *\r\nConnection: Close\r\n\r\n", relayType.c_str());
    const uint32_t head = relayHead;
    l.c = c;
    l.pos = head - min(head, (uint32_t)RELAY_START);
    l.gen = relayGen;
    l.used = true;
    Serial.printf("[live] auditeur connecté depuis %s\n", c.remoteIP().toString().c_str());
    return;
  }
  c.print("HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: Close\r\n"
          "Access-Control-Allow-Origin: *\r\n\r\nTrop d'auditeurs\r\n");
  c.stop();
}

void reply(WiFiClient& c, const char* status, const char* extra = "") {
  c.printf("HTTP/1.0 %s\r\nServer: Icecast 2.4.4\r\nConnection: Close\r\nContent-Length: 0\r\n%s\r\n", status, extra);
  c.stop();
}

String urlDecode(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch == '+') {
      out += ' ';
    } else if (ch == '%' && i + 2 < s.length()) {
      char hex[3] = {s[i + 1], s[i + 2], 0};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += ch;
    }
  }
  return out;
}

String queryParam(const String& path, const char* key) {
  int q = path.indexOf('?');
  if (q < 0) return "";
  String qs = "&" + path.substring(q + 1);
  String k = String("&") + key + "=";
  int p = qs.indexOf(k);
  if (p < 0) return "";
  int e = qs.indexOf('&', p + 1);
  return urlDecode(qs.substring(p + k.length(), e < 0 ? qs.length() : e));
}

// "Basic base64(utilisateur:motdepasse)" -> motdepasse
String basicPassword(const String& auth) {
  if (!auth.startsWith("Basic ")) return "";
  String b64 = auth.substring(6);
  b64.trim();
  unsigned char buf[128];
  size_t n = 0;
  if (mbedtls_base64_decode(buf, sizeof(buf) - 1, &n, (const unsigned char*)b64.c_str(), b64.length()) != 0) return "";
  buf[n] = 0;
  String up = (const char*)buf;
  int colon = up.indexOf(':');
  return colon >= 0 ? up.substring(colon + 1) : up;
}

void handle(WiFiClient c) {
  c.setTimeout(3000);  // ms (Stream::setTimeout) pour la lecture des en-têtes
  String line = c.readStringUntil('\n');
  line.trim();
  int s1 = line.indexOf(' '), s2 = line.indexOf(' ', s1 + 1);
  if (s1 < 0) return reply(c, "400 Bad Request");
  String method = line.substring(0, s1);
  String path = s2 > s1 ? line.substring(s1 + 1, s2) : line.substring(s1 + 1);

  String auth, ctype, expect, te, iceName;
  for (int i = 0; i < 40; i++) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.isEmpty()) break;
    int colon = h.indexOf(':');
    if (colon < 0) continue;
    String k = h.substring(0, colon), v = h.substring(colon + 1);
    k.toLowerCase();
    v.trim();
    if (k == "authorization") auth = v;
    else if (k == "content-type") ctype = v;
    else if (k == "expect") expect = v;
    else if (k == "transfer-encoding") te = v;
    else if (k == "ice-name") iceName = v;
  }

  String given = basicPassword(auth);
  if (given.isEmpty()) given = queryParam(path, "pass");
  const bool authOk = given == pass;

  // Mise à jour du titre envoyée par le logiciel de diffusion
  if (method == "GET" && path.startsWith("/admin/metadata")) {
    if (!authOk) return reply(c, "401 Unauthorized", "WWW-Authenticate: Basic realm=\"Icecast2 Server\"\r\n");
    String song = queryParam(path, "song");
    if (song.length()) Player::setLiveTitle(song);
    c.print("HTTP/1.0 200 OK\r\nServer: Icecast 2.4.4\r\nContent-Type: text/xml\r\nConnection: Close\r\n\r\n"
            "<?xml version=\"1.0\"?>\n<iceresponse><message>Metadata update successful</message><return>1</return></iceresponse>\r\n");
    c.stop();
    return;
  }

  // Écoute dans un navigateur : GET /stream (ou n'importe quel point de montage)
  if (method == "GET") return addListener(c);
  if (method != "SOURCE" && method != "PUT") return reply(c, "404 Not Found");
  if (!authOk) {
    Serial.println("[live] connexion refusée : mot de passe incorrect");
    return reply(c, "401 Unauthorized", "WWW-Authenticate: Basic realm=\"Icecast2 Server\"\r\n");
  }
  if (Player::liveBlocked()) return reply(c, "403 Forbidden");

  String ct = ctype;
  ct.toLowerCase();
  const bool aac = ct.indexOf("aac") >= 0;
  if (ct.length() && ct.indexOf("mpeg") < 0 && ct.indexOf("mp3") < 0 && !aac) {
    Serial.printf("[live] format refusé : %s (MP3 ou AAC attendu)\n", ctype.c_str());
    return reply(c, "415 Unsupported Media Type");
  }

  String e = expect;
  e.toLowerCase();
  if (e.indexOf("100-continue") >= 0) c.print("HTTP/1.1 100 Continue\r\n\r\n");
  else c.print(kOk);

  String tl = te;
  tl.toLowerCase();
  String mount = path;
  int q = mount.indexOf('?');
  if (q >= 0) mount = mount.substring(0, q);
  String name = iceName.length() ? iceName : String("PC") + (mount.length() > 1 ? " (" + mount.substring(1) + ")" : "");
  Serial.printf("[live] source connectée depuis %s : %s, %s\n", c.remoteIP().toString().c_str(), name.c_str(),
                aac ? "AAC" : "MP3");
  if (!Bt::connected()) Serial.println("[live] enceinte non connectée : relais vers les navigateurs uniquement");
  relayType = aac ? "audio/aac" : "audio/mpeg";
  Player::startLive(c, tl.indexOf("chunked") >= 0, aac, name.substring(0, 60));
}
}  // namespace

namespace Live {

void begin() {
  on = Storage::getBool("live_on", true);
  listenPort = Storage::getInt("live_port", 8000);
  pass = Storage::getString("live_pass", "radiobt");
  if (!on) return;
  if (!relay) relay = (uint8_t*)heap_caps_malloc(RELAY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!relay) Serial.println("[live] PSRAM indisponible : écoute dans le navigateur désactivée");
  server = new WiFiServer(listenPort);
  server->begin();
  Serial.printf("[live] source Icecast acceptée sur le port %d\n", listenPort);
}

void restart() {
  for (auto& l : listeners)
    if (l.used) dropListener(l);
  if (server) {
    server->end();
    delete server;
    server = nullptr;
  }
  begin();
}

void loop() {
  if (!server) return;
  WiFiClient c = server->accept();
  if (c) handle(c);
  serviceListeners();
}

void relayStart() {
  relayGen = relayGen + 1;  // déconnecte les auditeurs d'une diffusion précédente
  relayActive = true;
}

void relayStop() {
  relayActive = false;
  relayGen = relayGen + 1;
}

void relayPush(const uint8_t* data, size_t len) {
  if (!relay || !relayActive || !len) return;
  if (len > RELAY_SIZE) {
    data += len - RELAY_SIZE;
    len = RELAY_SIZE;
  }
  uint32_t head = relayHead;
  size_t off = head % RELAY_SIZE;
  size_t first = min(len, RELAY_SIZE - off);
  memcpy(relay + off, data, first);
  if (first < len) memcpy(relay, data + first, len - first);
  relayHead = head + len;  // publié après la copie
}

int listenerCount() {
  int n = 0;
  for (auto& l : listeners) n += l.used;
  return n;
}

bool enabled() { return on; }
int port() { return listenPort; }
String password() { return pass; }

}  // namespace Live
