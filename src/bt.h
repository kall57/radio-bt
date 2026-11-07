#pragma once
#include <Arduino.h>
#include <vector>

#include "BluetoothA2DPSource.h"

// Bluetooth A2DP (source) : découverte, connexion et reconnexion à l'enceinte.
namespace Bt {

struct Device {
  String name;
  String mac;
  int rssi;
  uint32_t lastSeen;
};

void begin(int32_t (*pull)(Frame* frames, int32_t count));
bool connected();
void loop();  // à appeler régulièrement depuis loop()

// "connected", "connecting", "searching" (enceinte enregistrée, en recherche)
// ou "unpaired" (aucune enceinte enregistrée)
const char* state();

String targetName();
String targetMac();
bool discovering();
std::vector<Device> devices();

// Enregistre l'enceinte choisie (prise en compte au redémarrage)
void select(const String& name, const String& mac);
void forget();

}  // namespace Bt
