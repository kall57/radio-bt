# Historique des versions

## 1.6.0
- Source **SDR++** : réception directe du son brut en TCP (`pcm://IP:port?rate=48000&ch=1`)
- **Mise à jour par le Wi-Fi** via un module de mise à jour (environnement `updater`) et une nouvelle table de partitions
- Correctif d'affichage des éléments masqués de l'interface

## 1.5.0
- Bouton casque : écoute de la station dans le navigateur
- Relais du flux Icecast vers 2 navigateurs (`:8000/stream`)
- La diffusion depuis un PC continue sans enceinte (relais seul, sans décodage)

## 1.4.0
- La passerelle accepte une source **Icecast** (BUTT, Mixxx, FFmpeg) : titre, retour à la station précédente, priorité au choix manuel

## 1.3.0
- Recherche de radios dans l'annuaire radio-browser.info
- Favoris (étoile, en tête du cadran, filtre « Favoris »)

## 1.2.x
- **Coupures audio corrigées** : les petites allocations vont en PSRAM, la RAM interne reste disponible pour le Wi-Fi et le Bluetooth
- Connexion Wi-Fi plus robuste (3 essais, meilleure borne, raison des échecs affichée)
- Diagnostic périodique (mémoire, tampon, débit, coupures)

## 1.1.0
- Recherche Bluetooth bloquée pendant la lecture

## 1.0.x
- Première version : webradio MP3/AAC vers enceinte Bluetooth, interface web, portail Wi-Fi
- Correctifs de démarrage (objets audio créés à l'exécution, réglages du moniteur série)
