#include "player.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecAACHelix.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/Communication/AudioHttp.h"
#include "bt.h"
#include "config.h"
#include "live.h"

using namespace audio_tools;

namespace {

// ---------------------------------------------------------------------------
// État partagé entre la tâche audio, la pile Bluetooth et le serveur web
// ---------------------------------------------------------------------------
enum class St { Stopped, WaitingBt, Connecting, Buffering, Playing, Retrying, Relaying };

SemaphoreHandle_t mtx;
StreamBufferHandle_t pcm;
StaticStreamBuffer_t pcmStruct;
size_t pcmSize = PCM_BUFFER_BYTES;
size_t prebufferBytes;

String reqUrl, reqName;                           // demande en attente
String curUrl, curName, curTitle, curCodec, lastError;
volatile bool wanted = false;
volatile bool changeReq = false;
volatile bool streaming = false;
volatile bool prebuffering = true;
volatile St st = St::Stopped;
volatile int vol = 70;
volatile int srcRate = 0, srcCh = 0, bitrate = 0;
volatile uint32_t underruns = 0, startedAt = 0;
volatile uint32_t netBytes = 0;   // octets reçus du serveur (mesure du débit)
volatile int netKbps = 0;

// Source directe (Icecast) : connexion transmise par live.cpp
volatile bool liveReq = false;     // nouvelle source en attente
volatile bool live = false;        // la lecture en cours vient de la source directe
volatile bool decoding = false;    // le décodeur alimente l'enceinte (faux : relais seul, sans enceinte)
bool liveAac = false;
uint8_t* relayScratch = nullptr;   // lecture de la source quand on ne décode pas
volatile uint32_t liveBlockUntil = 0;
WiFiClient pendingClient;
bool pendingChunked = false, pendingAac = false;
String pendingName;
String prevUrl, prevName;          // station à reprendre quand la source se déconnecte
bool prevWanted = false;

struct Guard {
  Guard() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(mtx); }
};

bool mustAbort() { return changeReq || liveReq || !wanted || !Bt::connected(); }

// ---------------------------------------------------------------------------
// Flux reçu d'une source Icecast : lecture brute ou "chunked" (HTTP/1.1)
// ---------------------------------------------------------------------------
class LiveStream : public Stream {
 public:
  void attach(WiFiClient c, bool isChunked) {
    client = c;
    client.setTimeout(2000);  // ms : lecture des en-têtes de bloc "chunked"
    chunked = isChunked;
    chunkLeft = 0;
    done = false;
  }
  void close() { client.stop(); }
  bool finished() { return done || (!client.connected() && client.available() == 0); }

  int available() override {
    int a = client.available();
    if (!chunked || a <= 0) return a;
    return chunkLeft > 0 ? min(a, (int)chunkLeft) : 1;  // 1 : un en-tête de bloc est à lire
  }
  // Tout octet lu est aussi copié vers le relais des navigateurs
  size_t readBytes(uint8_t* buf, size_t len) override {
    if (!chunked) {
      int n = client.read(buf, len);
      if (n <= 0) return 0;
      Live::relayPush(buf, n);
      return n;
    }
    if (chunkLeft == 0 && !nextChunk()) return 0;
    int n = client.read(buf, min(len, chunkLeft));
    if (n <= 0) return 0;
    chunkLeft -= n;
    if (chunkLeft == 0) client.readStringUntil('\n');  // CRLF de fin de bloc
    Live::relayPush(buf, n);
    return n;
  }
  size_t readBytes(char* buf, size_t len) override { return readBytes((uint8_t*)buf, len); }
  int read() override {
    uint8_t b;
    return readBytes(&b, 1) == 1 ? b : -1;
  }
  int peek() override { return -1; }
  size_t write(uint8_t) override { return 0; }

 private:
  WiFiClient client;
  bool chunked = false, done = false;
  size_t chunkLeft = 0;

  bool nextChunk() {
    String h = client.readStringUntil('\n');
    h.trim();
    if (h.isEmpty()) return false;
    chunkLeft = strtoul(h.c_str(), nullptr, 16);
    if (chunkLeft == 0) done = true;  // bloc de taille 0 : fin du flux
    return chunkLeft > 0;
  }
};

// ---------------------------------------------------------------------------
// Sortie du décodeur : mono -> stéréo, rééchantillonnage linéaire -> 44,1 kHz,
// puis écriture dans le tampon circulaire (en PSRAM)
// ---------------------------------------------------------------------------
class PcmSink : public AudioOutput {
 public:
  void reset() {
    havePrev = false;
    pos = 0.0f;
    carryLen = 0;
    outLen = 0;
    dropping = false;
  }

  void setAudioInfo(AudioInfo info) override {
    AudioOutput::setAudioInfo(info);
    srcRate = info.sample_rate;
    srcCh = info.channels;
    step = (info.sample_rate > 0) ? (float)info.sample_rate / OUTPUT_RATE : 1.0f;
    Serial.printf("[audio] flux : %d Hz, %d canal(aux)\n", (int)info.sample_rate, (int)info.channels);
  }

  size_t write(const uint8_t* data, size_t len) override {
    dropping = false;
    const int ch = (srcCh == 1) ? 1 : 2;
    const size_t fb = ch * 2;
    size_t i = 0;

    // fin d'une trame coupée lors de l'appel précédent
    while (carryLen > 0 && i < len) {
      carry[carryLen++] = data[i++];
      if (carryLen == fb) {
        frame(carry, ch);
        carryLen = 0;
      }
    }
    for (; i + fb <= len && !dropping; i += fb) frame(data + i, ch);
    if (!dropping)
      while (i < len) carry[carryLen++] = data[i++];

    flush();
    return len;
  }

 private:
  static constexpr size_t OUT_FRAMES = 256;
  int16_t out[OUT_FRAMES * 2];
  size_t outLen = 0;  // en trames
  uint8_t carry[4];
  size_t carryLen = 0;
  float step = 1.0f, pos = 0.0f;
  int16_t pL = 0, pR = 0;
  bool havePrev = false;
  bool dropping = false;

  void frame(const uint8_t* p, int ch) {
    int16_t s[2];
    memcpy(s, p, ch * 2);  // memcpy : pas d'accès 16 bits non aligné
    int16_t L = s[0];
    int16_t R = (ch == 2) ? s[1] : s[0];

    if (srcRate == 0 || srcRate == OUTPUT_RATE) {
      emit(L, R);
      return;
    }
    if (!havePrev) {
      pL = L;
      pR = R;
      havePrev = true;
      return;
    }
    while (pos < 1.0f) {
      emit((int16_t)(pL + (L - pL) * pos), (int16_t)(pR + (R - pR) * pos));
      pos += step;
    }
    pos -= 1.0f;
    pL = L;
    pR = R;
  }

  void emit(int16_t L, int16_t R) {
    out[outLen * 2] = L;
    out[outLen * 2 + 1] = R;
    if (++outLen == OUT_FRAMES) flush();
  }

  void flush() {
    if (outLen == 0 || dropping) {
      outLen = 0;
      return;
    }
    const uint8_t* d = (const uint8_t*)out;
    size_t len = outLen * 4, sent = 0;
    outLen = 0;
    while (sent < len) {
      if (mustAbort()) {
        dropping = true;
        return;
      }
      sent += xStreamBufferSend(pcm, d + sent, len - sent, pdMS_TO_TICKS(100));
    }
  }
};

// Objets audio créés dans Player::begin() et NON en variables globales :
// leurs constructeurs utilisent l'allocateur d'audio-tools, qui n'est pas encore
// initialisé pendant la construction des globales (plantage avant setup()).
PcmSink* sinkP = nullptr;
ICYStream* urlP = nullptr;
MP3DecoderHelix* mp3P = nullptr;
AACDecoderHelix* aacP = nullptr;
EncodedAudioStream* decP = nullptr;
StreamCopy* copierP = nullptr;
LiveStream* liveP = nullptr;

// Source PCM brute (SDR++ "Network sink" en mode TCP) : pcm://hôte:port?rate=48000&ch=1
WiFiClient pcmClient;
volatile bool pcmMode = false;
uint8_t* pcmBuf = nullptr;
StreamCopy* liveCopierP = nullptr;

void setError(const String& e) {
  Guard g;
  lastError = e;
}

void onMetadata(MetaDataType type, const char* str, int len) {
  if (type != Title) return;
  Guard g;
  curTitle = String(str).substring(0, len);
  curTitle.trim();
}

// ---------------------------------------------------------------------------
// Listes de lecture .m3u / .pls : on récupère la première URL de flux
// ---------------------------------------------------------------------------
// pcm://192.168.1.20:7355?rate=48000&ch=1  (port 7355, 48000 Hz et mono par défaut)
bool parsePcmUrl(const String& u, String& host, uint16_t& port, int& rate, int& ch) {
  if (!u.startsWith("pcm://")) return false;
  String rest = u.substring(6), query;
  int q = rest.indexOf('?');
  if (q >= 0) {
    query = "&" + rest.substring(q + 1) + "&";
    rest = rest.substring(0, q);
  }
  if (rest.endsWith("/")) rest.remove(rest.length() - 1);
  int colon = rest.lastIndexOf(':');
  long p = 7355;
  if (colon >= 0) {
    p = rest.substring(colon + 1).toInt();
    rest = rest.substring(0, colon);
  }
  rate = 48000;
  ch = 1;
  int k = query.indexOf("&rate=");
  if (k >= 0) rate = query.substring(k + 6, query.indexOf('&', k + 1)).toInt();
  k = query.indexOf("&ch=");
  if (k >= 0) ch = query.substring(k + 4, query.indexOf('&', k + 1)).toInt();
  if (rest.isEmpty() || p < 1 || p > 65535 || rate < 8000 || rate > 96000 || (ch != 1 && ch != 2)) return false;
  host = rest;
  port = (uint16_t)p;
  return true;
}

bool looksLikePlaylist(const String& u) {
  String l = u;
  l.toLowerCase();
  int q = l.indexOf('?');
  if (q >= 0) l = l.substring(0, q);
  return l.endsWith(".m3u") || l.endsWith(".pls");
}

String resolvePlaylist(const String& u) {
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  secure.setInsecure();
  bool ok = u.startsWith("https") ? http.begin(secure, u) : http.begin(plain, u);
  if (!ok) return "";
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(6000);
  String found;
  if (http.GET() == HTTP_CODE_OK) {
    String body = http.getString().substring(0, 4096);
    int start = 0;
    while (start < (int)body.length() && found.isEmpty()) {
      int end = body.indexOf('\n', start);
      if (end < 0) end = body.length();
      String line = body.substring(start, end);
      line.trim();
      int eq = line.indexOf('=');
      if (line.startsWith("File") && eq > 0) line = line.substring(eq + 1);  // format .pls
      if (line.startsWith("http://") || line.startsWith("https://")) found = line;
      start = end + 1;
    }
  }
  http.end();
  return found;
}

// ---------------------------------------------------------------------------
void stopStream() {
  if (streaming) {
    if (live) liveP->close();
    else if (pcmMode) pcmClient.stop();
    else urlP->end();
    if (decoding && !pcmMode) decP->end();
  }
  decoding = false;
  pcmMode = false;
  streaming = false;
  xStreamBufferReset(pcm);
  prebuffering = true;
  startedAt = 0;
  Guard g;
  curTitle = "";
}

// SDR++ : on se connecte à son serveur TCP et le PCM part directement vers
// l'étage de conversion (mono -> stéréo, rééchantillonnage), sans décodeur.
bool startPcm(const String& u) {
  String host;
  uint16_t port;
  int rate, ch;
  if (!parsePcmUrl(u, host, port, rate, ch)) {
    setError("Adresse SDR++ invalide (exemple : pcm://192.168.1.20:7355?rate=48000&ch=1)");
    return false;
  }
  pcmClient.stop();
  if (!pcmClient.connect(host.c_str(), port, 3000)) {
    setError("SDR++ injoignable sur " + host + ":" + String(port) + " (sortie Network en mode TCP ?)");
    return false;
  }
  pcmClient.setNoDelay(true);
  AudioInfo ai(rate, ch, 16);
  sinkP->setAudioInfo(ai);
  bitrate = rate * ch * 16 / 1000;
  {
    Guard g;
    curCodec = "PCM";
    curTitle = "";
  }
  pcmMode = true;
  decoding = true;  // le "puits" PCM alimente l'enceinte (comptage des coupures)
  streaming = true;
  st = St::Buffering;
  Serial.printf("[audio] SDR++ %s:%u — PCM %d Hz, %s\n", host.c_str(), port, rate, ch == 1 ? "mono" : "stéréo");
  return true;
}

bool startStream() {
  String u;
  {
    Guard g;
    u = curUrl;
    lastError = "";
  }
  if (u.isEmpty()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    setError("Wi-Fi non connecté");
    return false;
  }

  st = St::Connecting;
  srcRate = 0;
  srcCh = 0;
  bitrate = 0;
  underruns = 0;
  sinkP->reset();

  if (u.startsWith("pcm://")) return startPcm(u);

  String lower = u;
  lower.toLowerCase();
  if (lower.indexOf(".m3u8") >= 0) {
    setError("Les flux HLS (.m3u8) ne sont pas pris en charge");
    return false;
  }
  if (looksLikePlaylist(u)) {
    String r = resolvePlaylist(u);
    if (r.isEmpty()) {
      setError("Liste de lecture illisible ou vide");
      return false;
    }
    Serial.printf("[audio] liste de lecture -> %s\n", r.c_str());
    u = r;
    lower = u;
    lower.toLowerCase();
  }

  const uint32_t heapBefore = ESP.getFreeHeap();
  if (!urlP->begin(u.c_str())) {
    setError("Serveur du flux injoignable");
    return false;
  }

  String type = urlP->getReplyHeader("Content-Type") ? urlP->getReplyHeader("Content-Type") : "";
  type.toLowerCase();
  bool isAac = type.indexOf("aac") >= 0 || type.indexOf("mp4") >= 0 || lower.endsWith(".aac");
  if (type.startsWith("text/") || type.indexOf("html") >= 0 || type.indexOf("mpegurl") >= 0 ||
      type.indexOf("scpls") >= 0) {
    urlP->end();
    setError("Ce lien ne renvoie pas un flux audio (" + type + ")");
    return false;
  }
  const char* br = urlP->getReplyHeader("icy-br");
  bitrate = br ? atoi(br) : 0;

  decP->setDecoder(isAac ? (AudioDecoder*)aacP : (AudioDecoder*)mp3P);
  decP->begin();
  decoding = true;
  {
    Guard g;
    curCodec = isAac ? "AAC" : "MP3";
    curTitle = "";
  }
  streaming = true;
  st = St::Buffering;
  Serial.printf("[audio] lecture %s (%s) — mémoire interne : %u -> %u octets\n", u.c_str(), isAac ? "AAC" : "MP3",
                (unsigned)heapBefore, (unsigned)ESP.getFreeHeap());
  return true;
}

// Passe sur la source directe (en gardant la station en cours pour la reprendre ensuite)
void beginLive() {
  if (!live) {
    Guard g;
    prevUrl = curUrl;
    prevName = curName;
    prevWanted = wanted;
  }
  stopStream();
  WiFiClient c;
  bool aac;
  {
    Guard g;
    c = pendingClient;
    pendingClient = WiFiClient();
    liveP->attach(c, pendingChunked);
    aac = pendingAac;
    curName = pendingName;
    curUrl = "";
    curCodec = aac ? "AAC" : "MP3";
    curTitle = "";
    lastError = "";
  }
  liveAac = aac;
  live = true;
  wanted = true;
  srcRate = 0;
  srcCh = 0;
  bitrate = 0;
  underruns = 0;
  decoding = false;  // démarré par la boucle si l'enceinte est là
  Live::relayStart();
  streaming = true;
  st = St::Buffering;
}

// Diffusion directe : on ne décode que si l'enceinte est connectée
void startLiveDecoding() {
  xStreamBufferReset(pcm);
  prebuffering = true;
  startedAt = 0;
  srcRate = 0;
  srcCh = 0;
  sinkP->reset();
  decP->setDecoder(liveAac ? (AudioDecoder*)aacP : (AudioDecoder*)mp3P);
  decP->begin();  // le décodeur se resynchronise tout seul sur la trame suivante
  decoding = true;
  Serial.println("[live] enceinte présente : lecture sur l'enceinte");
}

void stopLiveDecoding() {
  decP->end();
  decoding = false;
  xStreamBufferReset(pcm);
  prebuffering = true;
  startedAt = 0;
  Serial.println("[live] enceinte absente : relais vers les navigateurs uniquement");
}

// Fin de la source directe : on reprend la station d'avant (si elle jouait)
void endLive(bool restore) {
  Live::relayStop();
  stopStream();
  live = false;
  if (restore) {
    Guard g;
    curUrl = prevUrl;
    curName = prevName;
    wanted = prevWanted && prevUrl.length();
  }
  Serial.println(restore ? "[live] source déconnectée : retour à la station précédente" : "[live] source arrêtée");
}

void audioTask(void*) {
  urlP->setMetadataCallback(onMetadata);
  urlP->httpRequest().reply().setAutoCreateLines(true);  // garde les en-têtes icy-*
  mp3P->addNotifyAudioChange(*sinkP);
  aacP->addNotifyAudioChange(*sinkP);

  uint32_t lastData = 0, nextTry = 0;
  int failures = 0;

  uint32_t kbpsAt = millis();
  for (;;) {
    const bool bt = Bt::connected();
    if (millis() - kbpsAt >= 5000) {
      netKbps = (int)((uint64_t)netBytes * 8 / (millis() - kbpsAt));  // octets/ms*8 = kbit/s
      netBytes = 0;
      kbpsAt = millis();
    }

    if (liveReq) {
      liveReq = false;
      beginLive();
      lastData = millis();
    }
    if (changeReq) {
      changeReq = false;
      if (live) endLive(false);
      stopStream();
      Guard g;
      curUrl = reqUrl;
      curName = reqName;
      lastError = "";
      nextTry = 0;
      failures = 0;
    }
    if (streaming && !wanted) {
      if (live) endLive(false);  // arrêt demandé pendant une diffusion : on reste arrêté
      else stopStream();
    } else if (streaming && !bt && !live) {
      stopStream();  // webradio : rien à faire sans enceinte
    }
    // La diffusion directe continue sans enceinte (relais vers les navigateurs)
    if (live && streaming) {
      if (bt && !decoding) startLiveDecoding();
      else if (!bt && decoding) stopLiveDecoding();
    }

    if (!streaming) {
      if (!wanted) {
        st = St::Stopped;
      } else if (!bt) {
        st = St::WaitingBt;
      } else if (millis() >= nextTry) {
        if (startStream()) {
          lastData = millis();
          failures = 0;
        } else {
          failures++;
          st = St::Retrying;
          nextTry = millis() + min(30000, 3000 * failures);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    size_t n;
    if (pcmMode) {  // SDR++ : PCM brut, aucun décodage
      n = 0;
      int a = pcmClient.available();
      if (a > 0) {
        int r = pcmClient.read(pcmBuf, min(a, 4096));
        if (r > 0) {
          sinkP->write(pcmBuf, r);  // le puits gère les trames coupées entre deux lectures
          n = r;
        }
      } else if (!pcmClient.connected()) {
        stopStream();
        setError("SDR++ a fermé la connexion");
        st = St::Retrying;
        nextTry = millis() + 2000;
        continue;
      }
    } else if (live && !decoding) {  // relais seul : on lit la source sans décoder
      int a = liveP->available();
      n = a > 0 ? liveP->readBytes(relayScratch, min(a, 2048)) : 0;
    } else {
      n = live ? liveCopierP->copy() : copierP->copy();
    }
    if (n > 0) {
      netBytes = netBytes + n;
      lastData = millis();
    } else if (live && (liveP->finished() || millis() - lastData > 8000)) {
      endLive(true);
      continue;
    } else if (millis() - lastData > 8000) {
      stopStream();
      setError("Flux interrompu");
      st = St::Retrying;
      nextTry = millis() + 2000;
      continue;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (live && !decoding) {
      st = St::Relaying;
    } else if (prebuffering) {
      st = St::Buffering;
    } else {
      st = St::Playing;
      if (!startedAt) startedAt = millis();
    }
  }
}

const char* stateName(St s) {
  switch (s) {
    case St::Stopped: return "stopped";
    case St::WaitingBt: return "waiting_bt";
    case St::Connecting: return "connecting";
    case St::Buffering: return "buffering";
    case St::Playing: return "playing";
    case St::Retrying: return "retrying";
    case St::Relaying: return "relaying";
  }
  return "stopped";
}

}  // namespace

namespace Player {

void begin() {
  mtx = xSemaphoreCreateMutex();

  uint8_t* storage = (uint8_t*)heap_caps_malloc(pcmSize + 1, MALLOC_CAP_SPIRAM);
  if (!storage) {
    Serial.println("[audio] PSRAM introuvable : tampon réduit à 32 Ko");
    pcmSize = 32 * 1024;
    storage = (uint8_t*)malloc(pcmSize + 1);
  }
  prebufferBytes = pcmSize / 2;
  pcm = xStreamBufferCreateStatic(pcmSize, 1, storage, &pcmStruct);

  sinkP = new PcmSink();
  urlP = new ICYStream(2048);
  mp3P = new MP3DecoderHelix();
  aacP = new AACDecoderHelix();
  decP = new EncodedAudioStream(sinkP, mp3P);
  copierP = new StreamCopy(*decP, *urlP, 2048);
  liveP = new LiveStream();
  relayScratch = (uint8_t*)malloc(2048);
  pcmBuf = (uint8_t*)malloc(4096);
  liveCopierP = new StreamCopy(*decP, *liveP, 2048);

  // (une pile en PSRAM n'est pas autorisée par le cœur Arduino précompilé)
  xTaskCreatePinnedToCore(audioTask, "audio", 10240, nullptr, 2, nullptr, 1);
}

void play(const String& u, const String& n) {
  if (live) liveBlockUntil = millis() + 120000;  // choix manuel prioritaire sur la source directe
  {
    Guard g;
    reqUrl = u;
    reqName = n;
  }
  wanted = true;
  changeReq = true;
}

void stop() {
  if (live) liveBlockUntil = millis() + 120000;  // sinon la source se reconnecte aussitôt
  wanted = false;
}

void startLive(WiFiClient& c, bool chunked, bool aac, const String& name) {
  {
    Guard g;
    pendingClient = c;
    pendingChunked = chunked;
    pendingAac = aac;
    pendingName = name;
  }
  liveBlockUntil = 0;
  liveReq = true;
}

void setLiveTitle(const String& t) {
  if (!live) return;
  Guard g;
  curTitle = t.substring(0, 200);
}

bool liveBlocked() { return liveBlockUntil && (int32_t)(liveBlockUntil - millis()) > 0; }

void setVolume(int pct) { vol = constrain(pct, 0, 100); }
int volume() { return vol; }

Info info() {
  Info i;
  i.state = stateName(st);
  i.wanted = wanted;
  i.sampleRate = srcRate;
  i.channels = srcCh;
  i.bitrate = bitrate;
  i.bufferPct = (int)(xStreamBufferBytesAvailable(pcm) * 100 / pcmSize);
  i.underruns = underruns;
  i.elapsedS = startedAt ? (millis() - startedAt) / 1000 : 0;
  i.volume = vol;
  i.netKbps = streaming ? netKbps : 0;
  i.live = live;
  Guard g;
  i.name = changeReq ? reqName : curName;
  i.url = changeReq ? reqUrl : curUrl;
  i.title = curTitle;
  i.codec = streaming ? curCodec : "";
  i.error = (st == St::Retrying) ? lastError : "";
  return i;
}

int32_t pullFrames(Frame* frames, int32_t count) {
  const size_t want = count * 4;
  if (prebuffering) {
    if (xStreamBufferBytesAvailable(pcm) < prebufferBytes) {
      memset(frames, 0, want);
      return count;
    }
    prebuffering = false;
  }
  size_t got = xStreamBufferReceive(pcm, frames, want, 0);
  if (got < want) {
    memset((uint8_t*)frames + got, 0, want - got);
    if (streaming && decoding) {
      prebuffering = true;
      underruns = underruns + 1;
    }
  }
  const int v = vol;
  if (v < 100) {
    const int32_t g = (v * v * 256) / 10000;  // courbe quadratique, plus naturelle à l'oreille
    int16_t* s = (int16_t*)frames;
    for (int32_t k = 0; k < count * 2; k++) s[k] = (int16_t)((s[k] * g) >> 8);
  }
  return count;
}

}  // namespace Player
