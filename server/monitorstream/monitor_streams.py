#!/usr/bin/env python3
import json
import logging
import os
import time

import requests
import telebot

TOKEN = os.getenv("TELEGRAM_BOT_TOKEN", "CHANGE_ME")
CHAT_ID = int(os.getenv("TELEGRAM_CHAT_ID", "0"))
HOST = os.getenv("PUBLIC_STREAM_HOST", "https://stream.radioss113.it")

MICS_OPUS = [
    "mic0.opus",
    "mic1.opus",
]
MICS_DELAYED = [
    "trabia/0",
    "trabia/1",
    "casteldaccia/0",
    "brolo/0",
]

INTERVAL = 2
DEBOUNCE = 3
STATE_FILE = os.getenv("MONITOR_STATE_FILE", "/home/radio/monitorstream/stream_states.json")

TIMESTAMP_PUSH_URL = os.getenv("TIMESTAMP_PUSH_URL", "http://127.0.0.1:8090/push")
TIMESTAMP_TOKEN = os.getenv("EVENTS_PUSH_TOKEN", "CHANGE_ME")

logging.basicConfig(
    format="%(asctime)s %(levelname)s %(message)s",
    datefmt="[%H:%M:%S]",
    level=logging.DEBUG,
)
logger = logging.getLogger()


def is_stream_online(url):
    try:
        r = requests.get(url, stream=True, timeout=(3, 3))
        ok = r.status_code == 200
        r.close()
        return ok
    except requests.RequestException:
        return False


def load_persistent():
    expected = MICS_OPUS + MICS_DELAYED
    if os.path.isfile(STATE_FILE):
        try:
            with open(STATE_FILE, "r") as f:
                data = json.load(f)
            if isinstance(data, dict):
                normalized = {m: bool(data.get(m, True)) for m in expected}
                unknown = sorted(set(data.keys()) - set(expected))
                if unknown:
                    logger.warning("Chiavi obsolete nel file stato ignorate: %s", unknown)
                return normalized
        except Exception:
            logger.warning("Impossibile leggere lo stato persistente, lo azzero")
    return {m: True for m in expected}


def save_persistent(state):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f)


def push_event(mic_opus, online):
    ev = {
        "ts_utc": int(time.time() * 1000),
        "mic": mic_opus,
        "model": "monitor",
        "label": "stream_up" if online else "stream_down",
        "confidence": 1.0,
    }
    try:
        r = requests.post(
            TIMESTAMP_PUSH_URL,
            headers={"x-auth-token": TIMESTAMP_TOKEN},
            json=ev,
            timeout=5,
        )
        r.raise_for_status()
        logger.info("Inviato evento a radio-timestamp: %s", ev)
    except Exception as e:
        logger.error("Errore push evento a radio-timestamp: %s", e)


def send_notice(bot, mic, is_online, phase):
    text = (
        f"{'✅ tornato online' if is_online else '⚠️ offline'}: <b>{mic}</b>\n"
        f"{HOST}/{mic}"
    )
    bot.send_message(CHAT_ID, text, parse_mode="HTML")
    logger.info(
        "Notified %s%s: %s",
        "ON" if is_online else "OFF",
        f" ({phase})" if phase else "",
        mic,
    )


def main():
    bot = telebot.TeleBot(TOKEN)
    logger.info("Monitor avviato")

    persistent = load_persistent()
    logger.info("Stato persistente caricato: %s", persistent)

    buffers = {}
    debounced = {}

    for m in MICS_OPUS:
        url = f"http://127.0.0.1:8001/{m}"
        buffers[m] = [is_stream_online(url)] * DEBOUNCE
        debounced[m] = all(buffers[m])

    for m in MICS_DELAYED:
        url = f"{HOST}/{m}"
        buffers[m] = [is_stream_online(url)] * DEBOUNCE
        debounced[m] = all(buffers[m])

    logger.info("Stato iniziale debounced: %s", debounced)

    for m in (MICS_OPUS + MICS_DELAYED):
        if debounced[m] != persistent.get(m, None):
            if m in MICS_DELAYED:
                send_notice(bot, m, debounced[m], "boot")
            else:
                push_event(m, debounced[m])

    persistent = debounced.copy()
    save_persistent(persistent)

    while True:
        for m in (MICS_OPUS + MICS_DELAYED):
            if m in MICS_OPUS:
                url = f"http://127.0.0.1:8001/{m}"
            else:
                url = f"{HOST}/{m}"

            raw = is_stream_online(url)
            buf = buffers[m]
            buf.append(raw)
            buf.pop(0)

            prev = debounced[m]
            if all(buf):
                curr = True
            elif not any(buf):
                curr = False
            else:
                curr = prev

            logger.debug("%s: raw=%s, buf=%s, prev=%s, curr=%s", m, raw, buf, prev, curr)

            if curr != prev:
                if m in MICS_DELAYED:
                    send_notice(bot, m, curr, "")
                else:
                    push_event(m, curr)

                debounced[m] = curr
                persistent[m] = curr
                save_persistent(persistent)

        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
