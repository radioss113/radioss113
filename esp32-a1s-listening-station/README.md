# ESP32 A1S Listening Station

Firmware microfono per `ESP32 Audio Kit A1S v2.2 / A541`.

## Varianti

- `single-mic-duplicated-lr`: usa un solo canale microfonico e lo duplica su `L/R`
- `dual-mic-stereo-lr`: usa entrambi i microfoni in stereo reale `left/right`

## Funzioni comuni

- configurazione persistente in NVS per Wi-Fi e stream Harbor
- captive portal HTTP con DNS redirect
- modalita' `STA` con fallback `AP`
- queue tra capture e sender Harbor
- invio `Opus/Ogg` verso `Liquidsoap/Icecast input.harbor`
- reset runtime con long-press `GPIO5`
- telemetria runtime

## Hardware comune

- `ESP32 Audio Kit A1S v2.2 / A541`
- codec `ES8388`
- `48 kHz`, `16 bit`
- board layer compatibile `CONFIG_ESP_LYRAT_V4_3_BOARD=y`
