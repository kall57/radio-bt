#pragma once
#include <Arduino.h>
#include <vector>

struct Station {
  String name;
  String url;
  bool fav = false;  // favori : affiché en tête du cadran
};

// Stations (LittleFS, /stations.json) et réglages (NVS / Preferences).
// À n'utiliser que depuis setup() et la boucle principale.
namespace Storage {
void begin();

const std::vector<Station>& stations();
bool saveStations(const std::vector<Station>& list);
void resetStations();

String getString(const char* key, const String& def = "");
void putString(const char* key, const String& value);
int getInt(const char* key, int def);
void putInt(const char* key, int value);
bool getBool(const char* key, bool def);
void putBool(const char* key, bool value);
void remove(const char* key);
}  // namespace Storage
