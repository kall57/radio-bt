#pragma once
#include <Arduino.h>
#include <WiFiClient.h>

#include "BluetoothA2DPSource.h"

// Lecture du flux webradio, décodage MP3/AAC, rééchantillonnage à 44,1 kHz
// et mise à disposition des échantillons pour l'A2DP.
namespace Player {

struct Info {
  const char* state;  // stopped | waiting_bt | connecting | buffering | playing | retrying
  bool wanted;        // l'utilisateur a demandé la lecture
  String name;
  String url;
  String title;       // titre en cours (métadonnées ICY)
  String codec;       // "MP3" / "AAC"
  String error;       // dernière erreur, si retrying
  int sampleRate;
  int channels;
  int bitrate;        // kbit/s annoncé par le serveur (icy-br), 0 si inconnu
  int bufferPct;
  uint32_t underruns;
  uint32_t elapsedS;
  int volume;
  int netKbps;        // débit réseau mesuré (kbit/s)
  bool live;          // lecture depuis une source directe (Icecast / BUTT)
};

void begin();
void play(const String& url, const String& name);
void stop();
void setVolume(int pct);
int volume();
Info info();

// Source directe (voir live.cpp)
void startLive(WiFiClient& client, bool chunked, bool aac, const String& name);
void setLiveTitle(const String& title);
bool liveBlocked();  // vrai pendant 2 min après un choix manuel

// Callback A2DP : fournit `count` trames stéréo 16 bits
int32_t pullFrames(Frame* frames, int32_t count);

}  // namespace Player
