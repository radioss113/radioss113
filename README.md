# Radio SS113

Repository monorepo per l'infrastruttura e i firmware del progetto `Radio SS113`.

## Componenti

- `server`: ingest audio, relay pubblico, monitoraggio, archiviazione rolling e analisi
- `mic-raspberry-pi4`: nodo Raspberry Pi 4 che rileva i microfoni e avvia istanze `ffmpeg`
- `esp32-a1s-listening-station`: firmware microfono per `ESP32 Audio Kit A1S`
- `esp32-a1s-playback-system`: firmware impianto di riproduzione per `ESP32 Audio Kit A1S`

## Struttura

```text
radio-ss113/
  README.md
  server/
  mic-raspberry-pi4/
  esp32-a1s-listening-station/
    single-mic-duplicated-lr/
    dual-mic-stereo-lr/
  esp32-a1s-playback-system/
    stable-2/
    stable-3/
```

## Note

- Le credenziali non devono essere versionate.
- Le configurazioni sensibili vanno spostate in file `.env`, env file `systemd` o secret file locali.
- Le registrazioni audio, i dump e i file generati in produzione vanno esclusi dal repository.
