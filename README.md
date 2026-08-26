# Radio SS113

Repository monorepo per l'infrastruttura e i firmware del progetto `Radio SS113`.

## Componenti

- `server`: ingest audio, relay pubblico, monitoraggio, archiviazione rolling e analisi
- `mic-raspberry-pi4`: nodo Raspberry Pi 4 che rileva i microfoni e avvia istanze `ffmpeg`
- `stazione-di-ascolto-esp32`: firmware microfono per `ESP32 Audio Kit A1S`
- `esp32-a1s-playback-system`: firmware impianto di riproduzione per `ESP32 Audio Kit A1S`

## Struttura

```text
radio-ss113/
  README.md
  server/
  mic-raspberry-pi4/
  stazione-di-ascolto-esp32/
    single-mic-duplicated-lr/
    dual-mic-stereo-lr/
  esp32-a1s-playback-system/
    stable-2/
    stable-3/
```
