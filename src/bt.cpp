#include "bt.h"

#include "config.h"
#include "storage.h"

namespace {
// La bibliothèque relance une recherche Bluetooth dès que la précédente se
// termine sans avoir trouvé l'enceinte, y compris quand l'enceinte est déjà
// connectée (elle ne répond alors plus aux recherches). Une recherche occupe
// la radio et provoque des coupures du son : on la bloque pendant la lecture.
class RadioSource : public BluetoothA2DPSource {
 public:
  void holdDiscovery(bool hold) { is_end = hold; }
};

RadioSource a2dp;
bool discoveryHeld = false;
SemaphoreHandle_t mtx = nullptr;
std::vector<Bt::Device> found;

String tName, tMac;
esp_bd_addr_t tAddr;
bool hasAddr = false;
volatile esp_a2d_connection_state_t connState = ESP_A2D_CONNECTION_STATE_DISCONNECTED;

String macToString(const uint8_t* a) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return String(b);
}

bool parseMac(const String& s, uint8_t* out) {
  unsigned int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

// Appelé par la pile Bluetooth pour chaque appareil audio découvert.
// Retourne true pour s'y connecter.
bool onDevice(const char* ssid, esp_bd_addr_t address, int rssi) {
  String mac = macToString(address);
  String name = ssid ? String(ssid) : String();

  if (mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
    bool updated = false;
    for (auto& d : found) {
      if (d.mac == mac) {
        if (name.length()) d.name = name;
        d.rssi = rssi;
        d.lastSeen = millis();
        updated = true;
        break;
      }
    }
    if (!updated) {
      if (found.size() >= 16) {
        size_t oldest = 0;
        for (size_t i = 1; i < found.size(); i++)
          if (found[i].lastSeen < found[oldest].lastSeen) oldest = i;
        found.erase(found.begin() + oldest);
      }
      found.push_back({name, mac, rssi, millis()});
    }
    xSemaphoreGive(mtx);
  }

  if (hasAddr) return mac.equalsIgnoreCase(tMac);
  return tName.length() > 0 && tName == name;
}

void onConnectionState(esp_a2d_connection_state_t state, void*) {
  connState = state;
  Serial.printf("[bt] %s\n", a2dp.to_str(state));
}
}  // namespace

namespace Bt {

void begin(int32_t (*pull)(Frame*, int32_t)) {
  mtx = xSemaphoreCreateMutex();
  tName = Storage::getString("bt_name");
  tMac = Storage::getString("bt_mac");
  hasAddr = parseMac(tMac, tAddr);

  a2dp.set_local_name(BT_LOCAL_NAME);
  a2dp.set_ssid_callback(onDevice);
  a2dp.set_on_connection_state_changed(onConnectionState);
  a2dp.set_data_callback_in_frames(pull);

  if (hasAddr) {
    a2dp.set_auto_reconnect(tAddr, 5);  // reconnexion directe, puis recherche
    Serial.printf("[bt] enceinte enregistrée : %s (%s)\n", tName.c_str(), tMac.c_str());
  } else {
    a2dp.set_auto_reconnect(false);
    Serial.println("[bt] aucune enceinte enregistrée : recherche en cours");
  }
  a2dp.start();
}

bool connected() { return a2dp.is_connected(); }

void loop() {
  const bool conn = a2dp.is_connected();
  if (conn && !discoveryHeld) {
    discoveryHeld = true;
    a2dp.holdDiscovery(true);
  } else if (!conn && discoveryHeld) {
    discoveryHeld = false;
    a2dp.holdDiscovery(false);
  }
  // une recherche en cours alors que l'enceinte est connectée : on l'arrête
  static uint32_t lastCancel = 0;
  if (conn && a2dp.is_discovery_active() && millis() - lastCancel > 2000) {
    lastCancel = millis();
    esp_bt_gap_cancel_discovery();
    Serial.println("[bt] recherche arrêtée (enceinte déjà connectée)");
  }
}

const char* state() {
  if (a2dp.is_connected()) return "connected";
  if (connState == ESP_A2D_CONNECTION_STATE_CONNECTING) return "connecting";
  if (!hasAddr && tName.isEmpty()) return "unpaired";
  return "searching";
}

String targetName() { return tName; }
String targetMac() { return tMac; }
bool discovering() { return a2dp.is_discovery_active(); }

std::vector<Device> devices() {
  std::vector<Device> copy;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    copy = found;
    xSemaphoreGive(mtx);
  }
  return copy;
}

void select(const String& name, const String& mac) {
  Storage::putString("bt_name", name);
  Storage::putString("bt_mac", mac);
}

void forget() {
  Storage::remove("bt_name");
  Storage::remove("bt_mac");
}

}  // namespace Bt
