# TTS

Questa cartella documenta il servizio `TTS` principale eseguito sul nodo `Raspberry Pi 4 TTS`.

Il servizio e' avviato tramite `systemd` come `gatewayttsesterno.service` e usa come entrypoint lo script `moduli_audio.py`.

## Cosa fa

Il gateway espone endpoint HTTP che restituiscono direttamente audio sintetizzato.

In pratica:

- riceve una richiesta come `/stazione.mp3` oppure `/stazione.wav`
- carica dinamicamente il modulo Python richiesto
- ottiene il testo da leggere
- invia il testo a un motore `TTS`
- restituisce il risultato come file audio

## Backend supportati

Il gateway e' stato progettato per lavorare con piu' motori di sintesi, tra cui:

- `Piper` tramite `Home Assistant`
- `Kokoro`
- `LocalAI`

## Parametri principali

Gli endpoint possono usare parametri HTTP per controllare la generazione:

- `testo`: testo esplicito da leggere, se non si vuole usare un modulo dinamico
- `engine`: motore TTS da usare
- `voce`: voce da selezionare
- `speed`: velocita' di lettura
- `intro`: uno o piu' audio introduttivi da anteporre

## Modulo `stazione`

Il modulo `stazione.py` genera un annuncio vocale che descrive una stazione di ascolto del progetto.

### Cosa fa

- legge il nome del microfono dalla query string
- legge il luogo richiesto
- associa il luogo a coordinate predefinite
- interroga `Open-Meteo` in tempo reale
- costruisce una frase con nome della stazione, meteo, temperatura e vento

### Luoghi previsti

Il modulo gestisce un insieme di luoghi predefiniti associati a coordinate locali.

Se il luogo non e' riconosciuto, usa una stazione di fallback configurata nel modulo.

### Parametri del modulo

- `mic` oppure `microfono`: nome del microfono da annunciare
- `luogo`: nome della stazione da usare per il meteo

### Esempi di utilizzo

- `/stazione.mp3?luogo=stazione_a`
- `/stazione.wav?luogo=stazione_b&mic=canale_principale`
- `/stazione.mp3?luogo=stazione_default&microfono=canale_ambiente`

### Frase generata

Il messaggio prodotto segue questa logica:

- annuncia il microfono richiesto
- annuncia il luogo
- aggiunge descrizione meteo
- aggiunge temperatura
- aggiunge intensita' e direzione del vento

Se il servizio meteo non risponde, il modulo restituisce una frase di fallback con le sole informazioni di stazione e microfono.

## Pubblicazione

- non pubblicare token, IP privati o credenziali reali dei backend TTS
- se in futuro si aggiunge il codice del gateway, va prima ripulito da secret e configurazioni locali
