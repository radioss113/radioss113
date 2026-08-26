# Single Mic Duplicated L/R

Versione della stazione di ascolto ESP32 pensata per usare un solo microfono.

Il segnale acquisito viene duplicato sui due canali dello stream, quindi in uscita si ottiene un flusso stereo compatibile con molte pipeline audio, ma con contenuto identico su sinistra e destra.

## Profilo

- stream `Opus/Ogg`
- `48 kHz`
- `16 bit`
- `2` canali in uscita
- contenuto audio identico su `L` e `R`

## Comportamento

- acquisisce un solo ingresso microfonico
- duplica il segnale su `L` e `R`
- compatibile con pipeline stereo lato server
- non produce stereo reale

## File di riferimento

- progetto snapshot `stable_a1s_mic2_led_gpio22_2026-08-18-224106`
- `main/main.c`
- `main/mic_source_lyrat_mini_onboard.c`
- `main/opus_ogg_streamer.c`
- `main/wifi_station.c`
