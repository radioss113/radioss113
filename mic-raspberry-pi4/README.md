# Mic Raspberry Pi4

Nodo Raspberry Pi 4 per la discovery dinamica dei microfoni e l'avvio delle istanze `ffmpeg` di invio stream.

## Ruolo

Il nodo:

- rileva le schede audio disponibili con `arecord -l`
- crea una istanza logica `micN` per ogni card trovata
- genera un file `.env` per ogni istanza
- riavvia i servizi `ffmpeg-stream@micN.service`
- verifica che i mountpoint remoti siano raggiungibili

## Componenti

- `discovery/`: script di discovery e generazione env
- `systemd/`: `ffmpeg-discovery-launch.service` e `ffmpeg-stream@.service`

## Pipeline audio

Ogni istanza `ffmpeg-stream@micN.service`:

- legge l'input ALSA da `plughw:${CARD},0`
- acquisisce a `2` canali e `48000 Hz`
- converte a mono
- applica `aresample=async=1` e `highpass=f=45`
- codifica in `Opus`
- invia lo stream a `Liquidsoap harbor`

## Note

- La password di ingest non deve essere versionata.
- Il nodo attuale risulta basato su template `systemd`, senza override locali emersi.
