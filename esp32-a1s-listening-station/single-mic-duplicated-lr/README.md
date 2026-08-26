# Single Mic Duplicated L/R

Versione stabile della stazione di ascolto A1S che usa un solo canale microfonico e lo duplica sui due canali dello stream stereo.

## Profilo

- stream `Opus/Ogg`
- `48 kHz`
- `16 bit`
- `2` canali in uscita
- contenuto audio identico su `L` e `R`

## Comportamento

- baseline semplice e collaudata
- compatibile con pipeline stereo lato server
- non produce stereo reale

## File di riferimento

- progetto snapshot `stable_a1s_mic2_led_gpio22_2026-08-18-224106`
- `main/main.c`
- `main/mic_source_lyrat_mini_onboard.c`
- `main/opus_ogg_streamer.c`
- `main/wifi_station.c`
