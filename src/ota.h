#pragma once
#include <Arduino.h>

// Mise à jour par Wi-Fi en deux temps (le firmware est trop gros pour deux
// emplacements complets) :
//   app0 = la radio ; app1 = petit module de mise à jour (src/updater/)
//   1) une seule fois : installer le module dans app1 depuis l'interface ;
//   2) ensuite : "Mettre à jour" redémarre sur le module, qui reçoit
//      firmware.bin, l'écrit dans app0 et redémarre sur la radio.
namespace Ota {

void begin();             // détecte la table de partitions et le module
bool partitionsReady();   // vrai si la table de partitions OTA est installée
bool updaterInstalled();  // vrai si le module est présent dans app1

// Installation du module (appelée par le serveur web, morceau par morceau)
bool updaterUploadStart(String& error);
bool updaterUploadWrite(const uint8_t* data, size_t len, String& error);
bool updaterUploadEnd(String& error);
void updaterUploadAbort();

bool rebootToUpdater();   // démarre sur le module au prochain redémarrage

}  // namespace Ota
