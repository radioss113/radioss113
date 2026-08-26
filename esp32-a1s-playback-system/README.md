# ESP32 A1S Playback System

Firmware impianto di riproduzione per `ESP32 Audio Kit A1S`.

## Varianti

- `stable-2`: baseline orientata a Wi-Fi stabile e captive portal affidabile
- `stable-3`: evoluzione con polling catalogo, fallback piu' robusti e funzioni di automazione operativa

## Funzioni comuni

- playback stream HTTP
- captive portal di configurazione
- salvataggio configurazione in NVS
- selezione stream
- gestione TTS
- controlli fisici tramite tasti e LED di stato
