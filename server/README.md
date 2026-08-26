# Server

Nodo centrale di `Radio SS113`.

## Ruolo

Gestisce:

- ricezione stream audio dai nodi remoti
- rilancio pubblico degli stream
- rilancio privato interno per analisi e servizi locali
- monitoraggio disponibilita' stream
- archiviazione rolling delle registrazioni
- analisi audio automatica con `YAMNet` e `BirdNET`
- persistenza eventi in `MariaDB`

## Componenti

- `liquidsoap/`: ricezione Harbor e relay audio
- `icecast/`: configurazione server pubblico
- `monitorstream/`: controllo stream e notifiche
- `segmenter/`: registrazione rolling a segmenti orari
- `timestamp/`: servizio eventi locale opzionale
- `yamnet/`: analisi eventi audio
- `birdnet/`: analisi bioacustica
- `systemd/`: unit file dei servizi

## Note

- `radio-timestamp` e il layer SSE possono essere trattati come opzionali.
- Le password `Icecast`, i token Telegram, le credenziali DB e gli altri secret vanno esternalizzati.
