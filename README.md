# Radio SS113

`Radio SS113` e' un'infrastruttura di ascolto, trasmissione, analisi e riproduzione del paesaggio sonoro.

Il repository raccoglie documentazione tecnica, configurazioni di esempio e firmware dei nodi che compongono il sistema.

## Il progetto

`Radio SS113` nasce come progetto di ricerca sul paesaggio sonoro della costa settentrionale della Sicilia lungo la `Strada Statale 113`, l'asse che collega Messina a Trapani passando per Palermo e molti centri costieri.

L'idea centrale e' ascoltare e documentare il rapporto tra ambiente costruito e ambiente acquatico, mettendo in relazione i suoni della strada, del mare, delle attivita' umane e delle presenze non umane che convivono lungo questo tratto di costa.

Attraverso registrazione continua, streaming e analisi automatica, il progetto costruisce una memoria sonora del territorio e rende possibile una riflessione sul rumore, sull'antropizzazione e sulle trasformazioni ecologiche del paesaggio.

Questo repository documenta la parte tecnica che rende possibile quel lavoro: acquisizione dei segnali, invio degli stream, pubblicazione, monitoraggio, analisi e riproduzione.

## Cosa fa

Il sistema permette di:

- acquisire audio da microfoni remoti
- inviare gli stream al server centrale
- pubblicare e rilanciare gli stream via `Icecast` e `Liquidsoap`
- monitorare la disponibilita' degli stream
- registrare facoltativamente le ultime ore e gli ultimi giorni in segmenti ordinati
- analizzare automaticamente l'audio con strumenti come `YAMNet` e `BirdNET`
- riprodurre stream e messaggi `TTS` su nodi `ESP32 A1S`

## A cosa serve

L'infrastruttura e' pensata per sostenere piu' livelli di utilizzo:

- ascolto remoto in tempo reale degli stream raccolti sul territorio
- confronto tra sorgenti diverse, come mare, strada e altri punti di ripresa
- archiviazione e consultazione di registrazioni e metadati
- analisi automatica di eventi sonori, fauna, traffico e agenti atmosferici
- uso in contesti espositivi o installativi, dove gli stream possono essere riprodotti nello spazio

## Architettura

In forma semplificata, il sistema e' composto da questi blocchi:

- `server/`: nodo centrale che riceve gli stream, li pubblica, li archivia e li analizza
- `mic-raspberry-pi4/`: nodo microfonico su `Raspberry Pi 4` che rileva i microfoni collegati e avvia automaticamente le istanze `ffmpeg`
- `raspberry-pi4-tts-node/`: nodo ausiliario con servizi `TTS` e altri componenti locali
- `stazione-di-ascolto-esp32/`: firmware per nodi microfonici su `ESP32 A1S`
- `esp32-a1s-playback-system/`: firmware per nodi di riproduzione audio su `ESP32 A1S`

Il flusso generale e' il seguente:

- i nodi microfonici acquisiscono audio sul territorio
- l'audio viene inviato al server centrale
- il server riceve, pubblica, monitora e analizza gli stream
- i nodi di riproduzione possono riprodurre stream e messaggi audio in altri punti del sistema

## Varianti principali

### Stazione di ascolto ESP32

- `single-mic-duplicated-lr`: acquisisce un solo microfono e duplica lo stesso segnale su sinistra e destra; e' utile quando serve compatibilita' con una pipeline stereo ma il contenuto audio resta mono
- `dual-mic-stereo-lr`: acquisisce due microfoni distinti e mantiene i due canali separati in stereo reale `left/right`

### Impianto di riproduzione ESP32

- `stable-2`: versione stabile di base, orientata all'affidabilita' generale, senza polling periodico dei mountpoint
- `stable-3`: estensione della base stabile che aggiunge polling periodico dei mountpoint e una gestione piu' autonoma della selezione degli stream

## Evoluzione del sistema

La piattaforma e' stata pensata per partire da una prima stazione di ascolto e potersi estendere in futuro ad altri punti distribuiti lungo la `SS113`.

Nel suo sviluppo completo, il sistema puo' integrare:

- piu' microfoni e piu' punti di ascolto
- dati meteorologici e misure derivate dall'audio
- strumenti di confronto tra diverse sorgenti acustiche
- archivi consultabili per ricerca, sound design e sperimentazione

## Struttura del repository

```text
radio-ss113/
  README.md
  server/
  mic-raspberry-pi4/
  raspberry-pi4-tts-node/
  stazione-di-ascolto-esp32/
    single-mic-duplicated-lr/
    dual-mic-stereo-lr/
  esp32-a1s-playback-system/
    stable-2/
    stable-3/
```
