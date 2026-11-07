#pragma once
#include <Arduino.h>

// Réception d'une source Icecast (BUTT, Mixxx, ffmpeg...) : la passerelle se
// comporte comme un serveur Icecast à une seule source et joue le flux reçu.
#ifndef LIVE_MAX_LISTENERS
#define LIVE_MAX_LISTENERS 2  // navigateurs pouvant écouter la diffusion en même temps
#endif

namespace Live {

void begin();   // démarre l'écoute selon les réglages (à appeler après le Wi-Fi)
void loop();    // accepte les connexions (boucle principale)
void restart(); // à appeler après un changement de réglage

bool enabled();
int port();
String password();

// Relais vers les navigateurs (appelé par le lecteur)
void relayStart();                                // début de diffusion
void relayStop();                                 // fin de diffusion : auditeurs déconnectés
void relayPush(const uint8_t* data, size_t len);  // octets reçus de la source (tâche audio)
int listenerCount();

}  // namespace Live
