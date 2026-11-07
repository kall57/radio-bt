#include "storage.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"

namespace {
Preferences prefs;
std::vector<Station> list;
bool fsOk = false;
const char* kFile = "/stations.json";

const Station kDefaults[] = {
    {"FIP", "http://icecast.radiofrance.fr/fip-midfi.mp3", true},
    {"France Inter", "http://icecast.radiofrance.fr/franceinter-midfi.mp3"},
    {"France Info", "http://icecast.radiofrance.fr/franceinfo-midfi.mp3"},
    {"France Culture", "http://icecast.radiofrance.fr/franceculture-midfi.mp3"},
    {"France Musique", "http://icecast.radiofrance.fr/francemusique-midfi.mp3"},
    {"Mouv'", "http://icecast.radiofrance.fr/mouv-midfi.mp3"},
};

void loadDefaults() {
  list.clear();
  for (const auto& s : kDefaults) list.push_back(s);
}

bool writeFile() {
  if (!fsOk) return false;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& s : list) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    o["url"] = s.url;
    if (s.fav) o["fav"] = true;
  }
  File f = LittleFS.open(kFile, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  return n > 0;
}
}  // namespace

namespace Storage {

void begin() {
  prefs.begin("radiobt", false);

  fsOk = LittleFS.begin(true);  // formate la partition au premier démarrage
  if (!fsOk) Serial.println("[storage] LittleFS indisponible : stations non persistantes");

  list.clear();
  if (fsOk && LittleFS.exists(kFile)) {
    File f = LittleFS.open(kFile, "r");
    JsonDocument doc;
    if (f && !deserializeJson(doc, f)) {
      for (JsonObject o : doc.as<JsonArray>()) {
        String name = o["name"] | "";
        String url = o["url"] | "";
        if (url.length()) list.push_back({name.length() ? name : url, url, o["fav"] | false});
      }
    }
    if (f) f.close();
  }
  if (list.empty()) {
    loadDefaults();
    writeFile();
  }
}

const std::vector<Station>& stations() { return list; }

bool saveStations(const std::vector<Station>& l) {
  list = l;
  return writeFile();
}

void resetStations() {
  loadDefaults();
  writeFile();
}

// isKey() évite les messages "NOT_FOUND" dans le moniteur quand un réglage n'existe pas encore
String getString(const char* key, const String& def) { return prefs.isKey(key) ? prefs.getString(key, def) : def; }
void putString(const char* key, const String& v) { prefs.putString(key, v); }
int getInt(const char* key, int def) { return prefs.isKey(key) ? prefs.getInt(key, def) : def; }
void putInt(const char* key, int v) { prefs.putInt(key, v); }
bool getBool(const char* key, bool def) { return prefs.isKey(key) ? prefs.getBool(key, def) : def; }
void putBool(const char* key, bool v) { prefs.putBool(key, v); }
void remove(const char* key) {
  if (prefs.isKey(key)) prefs.remove(key);
}

}  // namespace Storage
