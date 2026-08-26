# Stable 3

Evoluzione dell'impianto di riproduzione A1S costruita sopra la base stabile.

La differenza principale rispetto a `stable-2` e' l'aggiunta di un polling periodico dei mountpoint, usato per aggiornare in modo piu' autonomo la scelta delle sorgenti disponibili.

## Funzioni principali

- polling periodico del catalogo Icecast
- selezione dinamica stazione e mount
- mount sticky
- fallback configurabili
- watchdog Wi-Fi con reconnect forzato
- annuncio di startup
- TTS live o buffered

## Ruolo nel progetto

- aggiunge automazione operativa rispetto a `stable-2`
- controlla periodicamente lo stato dei mountpoint
- migliora il recupero automatico in caso di cambi di disponibilita' degli stream

## Controlli osservati

- LED di stato `GPIO23`
- pulsante TTS `GPIO21`
- pulsante stazione `GPIO19`
- pulsante mount `GPIO22`

## File di riferimento

- `player_test_catalog_polling_bg_task.c`
