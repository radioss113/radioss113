# Server

Il server e' il nodo centrale di `Radio SS113`.

Riceve gli stream provenienti dai nodi remoti, li rende disponibili ai client, li registra, li monitora e li analizza. E' quindi il punto in cui convergono ingest, distribuzione pubblica, archiviazione e analisi automatica.

## Cosa fa

- riceve stream audio da nodi `Raspberry Pi` ed `ESP32`
- pubblica gli stream verso l'esterno
- mantiene stream interni per servizi e analisi locali
- controlla la disponibilita' delle sorgenti
- salva registrazioni rolling suddivise per giorno e per ora
- esegue analisi automatiche dell'audio
- salva eventi e metadati nei servizi locali previsti

## Componenti principali

- `liquidsoap/`: ricezione Harbor e instradamento degli stream
- `icecast/`: configurazione del server di pubblicazione
- `monitorstream/`: monitoraggio degli stream e notifiche
- `segmenter/`: registrazione rolling in segmenti orari
- `timestamp/`: servizio locale opzionale per eventi e sincronizzazione
- `yamnet/`: classificazione automatica di eventi audio
- `birdnet/`: analisi bioacustica
- `systemd/`: unit file dei servizi di produzione

## Nota operativa

- `radio-timestamp` e il layer `SSE` sono opzionali
- password, token Telegram, credenziali database e altri secret vanno sempre esternalizzati
