# Radio BT

**Passerelle webradio Wi-Fi → enceinte Bluetooth, sur ESP32-CAM.**
L'ESP32 rejoint votre Wi-Fi, lit une webradio et l'envoie en Bluetooth à n'importe quelle enceinte.
Tout se pilote depuis une interface web, sur téléphone ou ordinateur.

*Wi-Fi internet radio to Bluetooth speaker gateway for the ESP32-CAM (AI Thinker), with a web interface. PlatformIO / Arduino-ESP32 3.x. Documentation in French.*

<p align="center">
  <img src="docs/ecoute-desk-light.png" alt="Interface : cadran de stations" width="640">
  <img src="docs/ecoute-mob-dark.png" alt="Interface sur téléphone, thème sombre" width="190">
</p>

## Fonctions

**Écoute**
- Cadran de stations façon tuner, favoris en tête, lecture/arrêt, station précédente/suivante, volume
- Titre en cours (métadonnées ICY), codec, débit, fréquence, remplissage du tampon
- Flux **MP3** et **AAC**, http et https, listes `.m3u` / `.pls`, conversion automatique vers 44,1 kHz
- Reprise de la lecture après une coupure de courant, reconnexion automatique du flux et de l'enceinte

**Stations**
- Ajout, modification, ordre, favoris ; stockées dans la flash
- **Recherche intégrée** dans l'annuaire [radio-browser.info](https://www.radio-browser.info) (nom, genre, pays, format)

**Autres sources**
- **Son d'un PC** : la passerelle se comporte comme un **serveur Icecast** (BUTT, Mixxx, FFmpeg…) — sans serveur à installer
- **SDR++** : réception directe du son brut (PCM) en TCP, sans PC intermédiaire

**Écoute ailleurs**
- Bouton casque : écouter la station sur le téléphone ou l'ordinateur
- Relais du flux du PC vers 2 navigateurs, même sans enceinte connectée

**Système**
- Portail de configuration Wi-Fi au premier démarrage, accès par `http://radio-bt.local`
- **Mise à jour du firmware par le Wi-Fi**
- Diagnostic : mémoire, tampon, débit réseau, coupures (interface et moniteur série)

<p align="center">
  <img src="docs/stations-recherche.png" alt="Stations et recherche radio-browser" width="560">
</p>

## Matériel

| | |
|---|---|
| Carte | **ESP32-CAM AI Thinker** (ESP32, 4 Mo de flash, PSRAM 4 ou 8 Mo) |
| Programmation | adaptateur USB-série 5 V (FTDI…) ou carte **ESP32-CAM-MB** |
| Alimentation | **5 V / 2 A minimum** — Wi-Fi et Bluetooth simultanés font redémarrer la carte (« brownout ») avec une alimentation faible |
| Antenne | antenne externe conseillée : la résistance 0 Ω près du connecteur IPEX doit être sur la position « antenne externe » |
| Enceinte | toute enceinte Bluetooth (A2DP) |

La caméra n'est pas utilisée. Toute carte ESP32 « classique » avec PSRAM devrait convenir
(pas les ESP32-S2/S3/C3, qui n'ont pas de Bluetooth Classic).

## Installation

1. Installez [VS Code](https://code.visualstudio.com/) et l'extension **PlatformIO**, puis ouvrez ce dossier.
2. Branchez l'ESP32-CAM. Avec un adaptateur FTDI, reliez **GPIO0 à GND** pendant le téléversement,
   puis retirez ce pont et appuyez sur **RST**.
3. **Upload** (ou `pio run -t upload`), puis le moniteur série (`pio device monitor`).

Les bibliothèques sont téléchargées automatiquement, aux versions figées dans `platformio.ini`.

> **Ne changez pas la version de la plateforme** (`55.03.32` = Arduino-ESP32 3.3.2) : les versions plus
> récentes du cœur prennent plus de mémoire IRAM et l'édition de liens échoue avec Wi-Fi + Bluetooth Classic.

## Premier démarrage

1. Sur votre téléphone, rejoignez le réseau Wi-Fi **RadioBT-Config** ; la page de configuration s'ouvre
   (sinon : `http://192.168.4.1`).
2. Choisissez votre réseau, saisissez le mot de passe, « Enregistrer et redémarrer ».
3. Revenez sur votre réseau habituel et ouvrez **`http://radio-bt.local`** (ou l'adresse IP affichée dans le
   moniteur série).
4. Onglet **Enceinte** : mettez l'enceinte en mode appairage, puis « Utiliser ».
5. Onglet **Écoute** : choisissez une station sur le cadran.

<p align="center"><img src="docs/enceinte.png" alt="Choix de l'enceinte Bluetooth" width="560"></p>

## Diffuser le son d'un PC (BUTT, Mixxx, FFmpeg)

La passerelle accepte une source Icecast : inutile d'installer un serveur.

Dans **BUTT** : *Settings > Main > Server > ADD*, type **Icecast**
- Adresse : `radio-bt.local` (ou l'IP de la passerelle), port `8000`
- Mot de passe : `radiobt` — **à changer** dans Réglages > Diffusion depuis un PC
- Point de montage : au choix, par exemple `pc.mp3`
- Audio : **MP3, 44 100 Hz, 128 ou 192 kbit/s** (le mono est le plus fiable)

Avec **FFmpeg** (exemple : sortie UDP de SDR++ réencodée en MP3) :

```
ffmpeg -f s16le -ar 48000 -ac 1 -i "udp://127.0.0.1:7355?overrun_nonfatal=1&fifo_size=1000000" -ar 44100 -c:a libmp3lame -b:a 128k -content_type audio/mpeg -ice_name "SDR++" -f mp3 icecast://source:radiobt@radio-bt.local:8000/sdr.mp3
```

Dès que la source se connecte, elle prend la main ; quand elle s'arrête, la station précédente reprend.
Si vous choisissez une station (ou ■) pendant une diffusion, la source est ignorée pendant 2 minutes.
Formats acceptés : MP3 et AAC.

La diffusion continue sans enceinte : la passerelle la relaie alors aux navigateurs, sans la décoder.

## Écouter SDR++ directement

Dans SDR++ : *Sinks* > votre radio > **Network**, protocole **TCP**, adresse `0.0.0.0`, port `7355`,
**48000 Hz**, *Stereo* décoché, puis **Start** (état « Listening »).
Ajoutez ensuite une station avec l'adresse :

```
pcm://IP-du-PC:7355?rate=48000&ch=1
```

- Le son arrive brut (PCM 16 bits, 768 kbit/s en mono 48 kHz) : aucun décodage, mais 6 fois plus de débit
  Wi-Fi qu'un MP3 — préférez le mono.
- SDR++ ne sert **qu'un client à la fois** : si la passerelle reste à 0 kbit/s, faites *Stop* puis *Start*
  dans SDR++ et relancez la station.
- Le mode UDP de SDR++ n'est pas pris en charge : ses paquets sont trop grands pour l'ESP32.
- Ce flux ne peut pas être écouté dans un navigateur (utilisez FFmpeg + Icecast pour cela).

## Écouter sur un téléphone ou un ordinateur

Le **bouton casque** fait jouer le son par le navigateur :
- **webradio** : le navigateur lit directement le flux de la radio (l'ESP32 n'est pas sollicité) ;
  ■ coupe alors l'enceinte si vous voulez écouter seulement sur l'appareil ;
- **diffusion d'un PC** : le navigateur lit le relais de la passerelle,
  `http://radio-bt.local:8000/stream` (2 auditeurs au plus ; aussi lisible dans VLC).

Comptez quelques secondes de décalage avec l'enceinte.

## Mise à jour par le Wi-Fi

Le firmware (~2,2 Mo) ne tient pas deux fois dans les 4 Mo de flash : la mise à jour passe par un petit
**module de mise à jour** (~1 Mo) installé dans un second emplacement.

1. **Une seule fois** : `pio run -e updater`, puis Réglages > Mise à jour du firmware >
   envoyez `.pio/build/updater/firmware.bin` (« Installer le module »).
2. **À chaque mise à jour** : `pio run`, puis Réglages > « Mettre à jour le firmware ». La passerelle redémarre
   sur le module ; envoyez `.pio/build/esp32cam/firmware.bin`. Elle revient seule sur la radio
   (et aussi au bout de 15 min sans action).

Ne téléversez jamais l'environnement `updater` par USB : il écraserait la radio (PlatformIO le refuse).

| Partition | Adresse | Taille |
|---|---|---|
| nvs (réglages) | 0x9000 | 20 Ko |
| otadata | 0xE000 | 8 Ko |
| app0 — radio | 0x10000 | 2,56 Mo |
| app1 — module de mise à jour | 0x2A0000 | 1,06 Mo |
| spiffs — LittleFS (stations) | 0x3B0000 | 320 Ko |

## LED rouge (au dos de la carte)

| État | Signification |
|---|---|
| Clignote vite | Mode configuration Wi-Fi |
| Clignote moyennement | Mode mise à jour |
| Clignote lentement | Pas d'enceinte connectée |
| Allumée | Lecture en cours |
| Éteinte | Enceinte connectée, à l'arrêt |

## Bon à savoir

- **Le Wi-Fi et le Bluetooth partagent la même radio et la même antenne.** Ça fonctionne très bien en MP3
  jusqu'à 192 kbit/s ; préférez le MP3 à l'AAC (plus lourd à décoder) et évitez le HE-AAC.
- **La RAM interne est la ressource rare** (le Wi-Fi et le Bluetooth en prennent l'essentiel) ; la passerelle
  place tout ce qu'elle peut en PSRAM. Le diagnostic (moniteur série, toutes les 10 s, et Réglages > Système)
  affiche la mémoire libre, le tampon, le débit et les coupures.
- Flux non pris en charge : HLS (`.m3u8`), Ogg, Opus, FLAC.
- Changer d'enceinte redémarre la passerelle (quelques secondes).
- Le mot de passe de diffusion et l'interface ne sont pas protégés au-delà de votre réseau local :
  n'exposez pas la passerelle sur Internet.

Exemple de ligne de diagnostic :

```
[diag] RAM interne 42316 o (min 38120, bloc max 31220) | PSRAM 3812 Ko | tampon 99% | coupures 0 | réseau 193 kbit/s | Wi-Fi -57 dBm | BT connected | playing
```

## Structure du code

```
platformio.ini         environnements "esp32cam" (radio) et "updater" (module de mise à jour)
partitions.csv         table de partitions avec emplacement de mise à jour
scripts/embed_web.py   compresse web/index.html dans le firmware avant chaque compilation
web/index.html         interface web (HTML, CSS et JS dans un seul fichier)
src/main.cpp           démarrage, LED, diagnostic
src/net.*              Wi-Fi, portail de configuration, mDNS
src/bt.*               Bluetooth A2DP : recherche, connexion, reconnexion
src/player.*           flux HTTP/ICY, PCM SDR++, décodage, rééchantillonnage, tampon PSRAM
src/live.*             source Icecast (BUTT, FFmpeg) et relais vers les navigateurs
src/web.*              serveur HTTP et API JSON
src/storage.*          stations (LittleFS) et réglages (NVS)
src/ota.*              mise à jour par le Wi-Fi (côté radio)
src/updater/           module de mise à jour
```

Pour modifier l'interface, éditez seulement `web/index.html` : `src/web_index.h` est régénéré à chaque compilation.

## API

| Méthode | Chemin | Corps |
|---|---|---|
| GET | `/api/status` | — |
| GET / PUT | `/api/stations` | `[{"name","url","fav"}]` |
| POST | `/api/stations/reset` | — |
| POST | `/api/play` | `{"index":0}` ou `{"url","name"}` |
| POST | `/api/stop` | — |
| POST | `/api/volume` | `{"volume":0-100}` |
| POST | `/api/settings` | `{"autoplay","liveEnabled","livePort","livePassword"}` |
| POST | `/api/bt/select` | `{"name","mac"}` |
| POST | `/api/bt/forget` | — |
| GET | `/api/wifi/scan` | — |
| POST | `/api/wifi` | `{"ssid","password"}` |
| POST | `/api/ota/updater` | fichier (multipart) |
| POST | `/api/ota/start` | — |
| POST | `/api/restart` | — |

Port de diffusion (8000 par défaut) : `SOURCE`/`PUT /<montage>` (source Icecast), `GET /admin/metadata`
(titre), `GET /stream` (écoute).

## Bibliothèques et licence

Ce projet utilise :
- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) — Apache 2.0
- [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) — GPL-3.0
- [codec-helix](https://github.com/pschatzmann/codec-helix) — GPL-3.0 (décodeurs Helix : RealNetworks RPSL/RCSL)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — MIT
- [Arduino-ESP32](https://github.com/espressif/arduino-esp32) via [pioarduino](https://github.com/pioarduino/platform-espressif32)

Le firmware intégrant des bibliothèques sous GPL-3.0, **Radio BT est distribué sous licence GPL-3.0**
(voir [LICENSE](LICENSE)). Annuaire de radios : [radio-browser.info](https://www.radio-browser.info).
