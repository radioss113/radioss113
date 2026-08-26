# ESP32 A1S Playback System

Questa sezione contiene il firmware dei nodi di riproduzione basati su `ESP32 Audio Kit A1S`.

Questi dispositivi si collegano alla rete, ricevono stream audio HTTP dal sistema `Radio SS113`, permettono la configurazione tramite captive portal e possono anche riprodurre messaggi `TTS`.

## Varianti

- `stable-2`: versione stabile di base, senza polling periodico dei mountpoint; punta soprattutto a una riproduzione affidabile e a una configurazione semplice
- `stable-3`: evoluzione della base stabile che aggiunge polling periodico dei mountpoint, gestione piu' autonoma della selezione degli stream e maggiore resilienza operativa

## Funzioni comuni

- riproduzione di stream HTTP
- captive portal di configurazione
- salvataggio della configurazione in `NVS`
- selezione della sorgente audio
- gestione di messaggi `TTS`
- controlli fisici tramite pulsanti e LED di stato
