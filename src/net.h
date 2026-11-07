#pragma once
#include <Arduino.h>

// Wi-Fi : connexion au réseau enregistré, sinon point d'accès de
// configuration "RadioBT-Config" avec portail captif.
namespace Net {
void begin();
void loop();

bool apMode();
bool connected();
String ssid();       // réseau enregistré
String ip();         // IP sur le réseau (ou du point d'accès en mode AP)
int rssi();
String lastError();  // raison du dernier échec de connexion, vide si aucun

// Recherche des réseaux : -1 = en cours, >= 0 = nombre de résultats
int scan();
String scanSsid(int i);
int scanRssi(int i);
bool scanSecure(int i);
void scanClear();

void saveCredentials(const String& ssid, const String& pass);
}  // namespace Net
