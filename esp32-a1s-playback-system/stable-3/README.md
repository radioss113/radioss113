# Stable 3

Evoluzione dell'impianto di riproduzione A1S con gestione piu' autonoma del catalogo, dei fallback e del recupero di rete.

## Funzioni principali

- polling periodico del catalogo Icecast
- selezione dinamica stazione e mount
- mount sticky
- fallback configurabili
- watchdog Wi-Fi con reconnect forzato
- annuncio di startup
- TTS live o buffered

## Controlli osservati

- LED di stato `GPIO23`
- pulsante TTS `GPIO21`
- pulsante stazione `GPIO19`
- pulsante mount `GPIO22`

## File di riferimento

- `player_test_catalog_polling_bg_task.c`
