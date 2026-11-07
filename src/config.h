#pragma once

#ifndef FW_VERSION
#define FW_VERSION "1.6.0"
#endif

// Réseau Wi-Fi par défaut (peut être défini dans platformio.ini).
// Vide = configuration via le portail "RadioBT-Config" au premier démarrage.
#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS ""
#endif

#define DEVICE_HOSTNAME   "radio-bt"        // http://radio-bt.local
#define BT_LOCAL_NAME     "Radio BT"
#define AP_SSID           "RadioBT-Config"  // point d'accès de configuration (ouvert)

#define WIFI_CONNECT_TIMEOUT_MS  20000
#define AP_RETRY_STA_MS          30000      // en mode AP, réessaie le Wi-Fi enregistré toutes les 30 s

// Tampon PCM 16 bits stéréo 44,1 kHz (~176 Ko/s) : 256 Ko en PSRAM ≈ 1,5 s
#define PCM_BUFFER_BYTES  (256 * 1024)
#define OUTPUT_RATE       44100             // fréquence imposée par l'A2DP

#define MAX_STATIONS      40

// Broches ESP32-CAM AI Thinker
#define FLASH_LED_PIN     4                 // grosse LED blanche : forcée éteinte
#define STATUS_LED_PIN    33                // petite LED rouge au dos (active à l'état bas)
