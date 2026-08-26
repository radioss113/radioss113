# Stazione Di Ascolto ESP32

Questa sezione contiene il firmware dei nodi microfonici basati su `ESP32 Audio Kit A1S v2.2 / A541`.

Questi nodi acquisiscono audio locale, si collegano alla rete Wi-Fi, espongono un portale di configurazione e inviano lo stream al server centrale tramite `Harbor`, usando codifica `Opus/Ogg`.

## Varianti

- `single-mic-duplicated-lr`: usa un solo microfono; il segnale viene duplicato su canale sinistro e destro, quindi lo stream risultante e' stereo solo come formato, ma il contenuto audio e' identico su `L` e `R`
- `dual-mic-stereo-lr`: usa due microfoni distinti e mantiene la separazione reale tra canale sinistro e destro; e' la variante da usare quando si vuole stereo reale `left/right`

## Funzioni comuni

- configurazione persistente in `NVS` per rete Wi-Fi e parametri di stream
- captive portal HTTP con redirect DNS per la configurazione iniziale
- funzionamento in modalita' `STA` con fallback `AP`
- acquisizione audio locale e invio continuo al server centrale
- codifica `Opus/Ogg` e invio verso `Liquidsoap` o `Icecast` lato server
- telemetria e stato runtime
- reset della configurazione tramite pressione prolungata del tasto previsto dal firmware

## Hardware comune

- `ESP32 Audio Kit A1S v2.2 / A541`
- codec audio `ES8388`
- acquisizione a `48 kHz`, `16 bit`
- supporto board compatibile `ESP LyraT`
