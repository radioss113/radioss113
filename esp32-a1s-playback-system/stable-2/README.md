# Stable 2

Versione stabile di base dell'impianto di riproduzione A1S.

Questa variante privilegia l'affidabilita' generale della connessione Wi-Fi, del captive portal e della riproduzione degli stream, senza introdurre un polling periodico dei mountpoint.

## Funzioni principali

- captive portal HTTP con DNS redirect
- salvataggio configurazione in NVS
- stream preset
- catalogo Icecast
- fallback configurabili
- TTS live o buffered

## Ruolo nel progetto

- base stabile e semplice da mantenere
- adatta quando non serve aggiornare periodicamente lo stato dei mountpoint
- riferimento per la variante `stable-3`

## Controlli osservati

- LED di stato `GPIO23`
- pulsante TTS `GPIO21`
- pulsante stazione `GPIO19`
- pulsante mount `GPIO22`

## File di riferimento

- `player_stable_2.c`
