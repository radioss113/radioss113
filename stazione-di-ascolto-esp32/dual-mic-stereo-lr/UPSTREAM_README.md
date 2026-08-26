# ESP32 Audio Kit A1S Stereo Harbor Mic

Firmware Radio SS113 per `ESP32 Audio Kit A1S v2.2 / A541`, ricostruito dalla versione stabile della stazione di ascolto LyraT Mini e portato sul codec `ES8388` con cattura stereo reale.

Stato corrente promosso: `stabile definitiva stereo` del `2026-08-24`.

## Stato attuale

Baseline corrente:

- target board: `ESP32 Audio Kit A1S v2.2 / A541`, compatibile con board layer ADF `CONFIG_ESP_LYRAT_V4_3_BOARD=y`
- MCU osservata: `ESP32-D0WD-V3`
- flash: `4 MB`
- PSRAM: rilevata in boot come `8 MB`
- codec audio: `ES8388`
- I2S: `I2S0`, `48 kHz`, `16 bit`, `stereo L/R`
- ingresso microfonico stabile corrente: `dual raw left/right`
- stream: `Opus/Ogg` stereo verso `Liquidsoap/Icecast input.harbor`
- bitrate Opus: `128 kbps`
- AP di configurazione: `Radio SS113 Alfredo`
- default URL harbor nel portale: `http://radioss113.it:8005/`
- default nome dispositivo nel portale: `test`
- reset runtime: long-press `GPIO5` per 5 secondi, cancella solo le chiavi runtime NVS

## Pin usati

Pin del board layer A1S/A541:

- I2C codec: SDA `GPIO33`, SCL `GPIO32`
- I2S codec: MCLK `GPIO0`, BCLK `GPIO27`, LRCK/WS `GPIO25`, DOUT `GPIO26`, DIN `GPIO35`
- microSD: CLK `GPIO14`, CMD `GPIO15`, D0 `GPIO2`, D1 `GPIO4`, D2 `GPIO12`, D3 `GPIO13`, detect `GPIO34`
- tasti standard: REC `GPIO36`, MODE `GPIO39`, touch SET/PLAY/VOL

Personalizzazioni firmware Radio SS113:

- reset lungo runtime: `GPIO5`
- LED di stato firmware: `GPIO22` attivo basso
- il LED onboard standard della board viene quindi riusato come LED di stato firmware

## Funzioni mantenute dalla stabile

- configurazione persistente in NVS per Wi-Fi e stream
- captive portal HTTP con DNS redirect
- fix scan Cudy: APSTA, scan hidden, sospensione reconnect STA durante scan
- boot Wi-Fi: STA con credenziali salvate, AP dopo grace timeout
- power-on settle di 3 secondi
- queue tra capture e sender Harbor
- recovery Harbor con reset stream Opus/Ogg a ogni nuova connessione
- watchdog flatline capture con recovery software automatico
- telemetria runtime su capture, queue, Wi-Fi, Harbor, Opus e livelli audio

## Logica LED corrente

- nessuna credenziale Wi-Fi salvata: blink `1s`
- credenziali salvate ma Wi-Fi non collegato: blink `2s`
- Wi-Fi collegato ma Harbor/config non stanno trasmettendo davvero: blink `2s`
- stream realmente in trasmissione verso Harbor: acceso fisso
- hard reset da `GPIO5`: blink rapido

## File principali

- `main/main.c`: orchestration, parametri audio/stereo, Harbor e task runtime
- `main/mic_source_lyrat_mini_onboard.c`: sorgente ES8388 A1S/A541 usata nel profilo `dual raw left/right`
- `main/opus_ogg_streamer.*`: pacchettizzazione Opus/Ogg con `channel_count=2`
- `main/wifi_station.*`: STA/APSTA, captive portal, reset GPIO5 e LED GPIO22
- `main/pcm_dc_blocker.*`: filtro DC con stato separato per canale stereo

## Documentazione

- [ULTIMA_VERSIONE_DEFINITIVA.txt](/Users/fortem/Documents/Codex/New%20project%202/ULTIMA_VERSIONE_DEFINITIVA.txt)
- [docs/STABLE_STEREO_DEFINITIVA_MANUAL.md](/Users/fortem/Documents/Codex/New%20project%202/test_branches/test_from_stable3_dual_mic_2026-08-20-133513/docs/STABLE_STEREO_DEFINITIVA_MANUAL.md)

## Build

```bash
cd /Users/fortem/Documents/esp/esp-adf
. ./export.sh
cd "/Users/fortem/Documents/Codex/New project 2"
idf.py build
```

Il binario viene generato come progetto `esp32_a1s_stereo_harbor_mic` e usa flash size `4MB`.
