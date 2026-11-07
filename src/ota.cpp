#include "ota.h"

#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace {
const esp_partition_t* app0 = nullptr;
const esp_partition_t* app1 = nullptr;
bool present = false;
size_t written = 0;

// Cherche le repère du module dans app1. Le repère est assemblé à l'exécution
// pour ne pas figurer en entier dans ce firmware-ci.
bool scanForUpdater() {
  if (!app1) return false;
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(app1, &desc) != ESP_OK) return false;  // emplacement vide
  String needle = String("RADIOBT-") + "UPDATER-V1";
  const size_t nlen = needle.length(), chunk = 4096;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(chunk + nlen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)malloc(chunk + nlen);
  if (!buf) return false;
  bool found = false;
  size_t keep = 0;  // recouvrement entre deux blocs
  for (size_t off = 0; off < app1->size && !found; off += chunk) {
    size_t n = min(chunk, (size_t)(app1->size - off));
    if (esp_partition_read(app1, off, buf + keep, n) != ESP_OK) break;
    size_t total = keep + n;
    found = memmem(buf, total, needle.c_str(), nlen) != nullptr;
    keep = min(nlen - 1, total);
    memmove(buf, buf + total - keep, keep);
  }
  free(buf);
  return found;
}
}  // namespace

namespace Ota {

void begin() {
  app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  app1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  // L'ancienne table (huge_app) n'a qu'un emplacement : pas de mise à jour Wi-Fi possible
  if (app1 && app1->size < 0x100000) app1 = nullptr;
  present = scanForUpdater();
  Serial.printf("[ota] table OTA : %s, module de mise à jour : %s\n", app1 ? "oui" : "non (ancienne table)",
                present ? "installé" : "absent");
}

bool partitionsReady() { return app0 && app1; }
bool updaterInstalled() { return present; }

bool updaterUploadStart(String& error) {
  written = 0;
  if (!partitionsReady()) {
    error = "Table de partitions sans emplacement de mise à jour : téléversez une fois cette version par USB";
    return false;
  }
  if (esp_ota_get_running_partition() != app0) {
    error = "La radio ne tourne pas depuis son emplacement normal";
    return false;
  }
  // Update écrit dans l'emplacement OTA suivant celui en cours : app1
  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    error = String("Préparation impossible : ") + Update.errorString();
    return false;
  }
  return true;
}

bool updaterUploadWrite(const uint8_t* data, size_t len, String& error) {
  if (written == 0 && len > 0 && data[0] != 0xE9) {
    error = "Ce fichier n'est pas un firmware ESP32";
    Update.abort();
    return false;
  }
  if (Update.write((uint8_t*)data, len) != len) {
    error = String("Écriture impossible : ") + Update.errorString();
    Update.abort();
    return false;
  }
  written += len;
  return true;
}

bool updaterUploadEnd(String& error) {
  if (!Update.end(true)) {
    error = String("Fichier refusé : ") + Update.errorString();
    return false;
  }
  // Update.end() a choisi app1 comme prochain démarrage : on revient sur la radio
  esp_ota_set_boot_partition(app0);
  present = scanForUpdater();
  if (!present) {
    error = "Ce fichier n'est pas le module de mise à jour (compilez l'environnement \"updater\")";
    return false;
  }
  Serial.printf("[ota] module de mise à jour installé (%u octets)\n", (unsigned)written);
  return true;
}

void updaterUploadAbort() { Update.abort(); }

bool rebootToUpdater() {
  if (!present || !app1) return false;
  return esp_ota_set_boot_partition(app1) == ESP_OK;
}

}  // namespace Ota
