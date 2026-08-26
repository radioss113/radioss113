# Raspberry Pi 4 TTS Node

Questa sezione documenta un nodo `Raspberry Pi 4` usato per servizi ausiliari del progetto `Radio SS113`.

Al momento, la parte documentata qui riguarda il servizio `TTS` principale, usato per generare messaggi vocali dinamici accessibili via HTTP.

## Ruolo

Il nodo ospita un gateway `TTS` che:

- riceve richieste HTTP
- genera testo dinamico tramite moduli Python dedicati
- inoltra la sintesi vocale a uno dei motori disponibili
- restituisce direttamente l'audio generato in formato `mp3` o `wav`

## Componenti documentati

- `tts/`: documentazione del gateway TTS e del modulo `stazione`

## Note

- le credenziali e i token dei backend TTS non devono essere pubblicati
- i file di backup locali e i moduli non usati in produzione non vanno inclusi nel repository senza revisione
