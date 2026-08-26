# Dual Mic Stereo L/R

Versione della stazione di ascolto ESP32 che usa due microfoni distinti in stereo reale.

Il canale sinistro e il canale destro restano separati lungo tutta la catena di acquisizione e streaming, permettendo di conservare la differenza spaziale tra i due segnali.

## Profilo

- stream `Opus/Ogg`
- `48 kHz`
- `16 bit`
- `2` canali in uscita
- canale sinistro mantenuto su `L`
- canale destro mantenuto su `R`

## Comportamento

- acquisisce due microfoni distinti
- mantiene la separazione `left/right`
- usa la stessa infrastruttura Wi-Fi, portal e Harbor della variante mono
- e' la variante consigliata quando serve stereo reale

## File di riferimento

- progetto snapshot `stabile_definitiva_stereo_2026-08-24`
- `README.md`
- documentazione `STABLE_STEREO_DEFINITIVA_MANUAL.md`
