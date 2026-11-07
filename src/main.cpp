/*
  Radio BT — passerelle webradio Wi-Fi -> enceinte Bluetooth
  Carte : ESP32-CAM AI Thinker (ESP32 + 4 Mo PSRAM ou plus)

  - storage : stations (LittleFS) et réglages (NVS)
  - net     : Wi-Fi, portail de configuration, mDNS
  - bt      : A2DP source, découverte et reconnexion à l'enceinte
  - player  : flux HTTP/ICY, décodage MP3/AAC, rééchantillonnage, tampon PSRAM
  - web     : interface web et API JSON
*/
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "bt.h"
#include "config.h"
#include "live.h"
#include "ota.h"
#include "net.h"
#include "player.h"
#include "storage.h"
#include "web.h"

static void updateStatusLed() {
  // allumée = lecture ; clignotement lent = pas d'enceinte ; rapide = mode configuration
  bool on;
  if (Net::apMode()) {
    on = (millis() / 150) % 2;
  } else if (!Bt::connected()) {
    on = (millis() / 800) % 2;
  } else {
    on = strcmp(Player::info().state, "playing") == 0;
  }
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);  // LED active à l'état bas
}

void setup() {
  // La RAM interne (~300 Ko) est presque entièrement prise par le Wi-Fi et le
  // Bluetooth. Par défaut, toute allocation < 4 Ko y est placée (tampons des
  // paquets Bluetooth, réseau, interface web...) jusqu'à l'épuiser -> coupures.
  // Au-delà de 512 octets, on utilise désormais la PSRAM (8 Mo, presque vide).
  heap_caps_malloc_extmem_enable(512);

  Serial.begin(115200);
  delay(200);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);

  Serial.printf("\nRadio BT %s — PSRAM libre : %u octets, RAM interne libre : %u octets\n", FW_VERSION,
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());

  Storage::begin();
  Ota::begin();
  Player::begin();
  Player::setVolume(Storage::getInt("volume", 70));
  Net::begin();
  Bt::begin(Player::pullFrames);
  Web::begin();
  Live::begin();

  String last = Storage::getString("last_url");
  // reprend la lecture si elle était en cours à la coupure de courant
  if (Storage::getBool("autoplay", true) && Storage::getBool("was_playing", false) && last.length()) {
    Player::play(last, Storage::getString("last_name", "Dernière station"));
  }
}

// Une ligne de diagnostic toutes les 10 s dans le moniteur série
static void printDiag() {
  Player::Info i = Player::info();
  Serial.printf("[diag] RAM interne %u o (min %u, bloc max %u) | PSRAM %u Ko | tampon %d%% | coupures %u | "
                "réseau %d kbit/s | Wi-Fi %d dBm | BT %s%s | %s\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)(ESP.getFreePsram() / 1024), i.bufferPct, (unsigned)i.underruns, i.netKbps, Net::rssi(),
                Bt::state(), Bt::discovering() ? " (recherche en cours)" : "", i.state);
}

void loop() {
  Net::loop();
  Web::loop();
  Bt::loop();
  Live::loop();

  static uint32_t lastDiag = 0;
  if (millis() - lastDiag > 10000) {
    lastDiag = millis();
    printDiag();
  }

  static uint32_t lastLed = 0;
  if (millis() - lastLed > 50) {
    lastLed = millis();
    updateStatusLed();
  }
  delay(2);
}
