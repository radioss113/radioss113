#!/usr/bin/env python3

import os
import re
import shutil
import subprocess
import time
from pathlib import Path

import requests

SERVER = os.getenv("STREAM_SERVER", "127.0.0.1")
STREAM_PORT = os.getenv("STREAM_PORT", "8005")
PASSWORD = os.getenv("STREAM_PASSWORD", "CHANGE_ME")
BITRATE = os.getenv("STREAM_BITRATE", "64k")
CHECK_BASE_URL = os.getenv("CHECK_BASE_URL", "https://pre.radioss113.it")
INST_DIR = Path(os.getenv("INSTANCE_ENV_DIR", "/etc/ffmpeg-stream/instances"))
REC_DIR = Path(os.getenv("RECORDINGS_DIR", "/var/recordings/ffmpeg"))
STREAM_USER = os.getenv("STREAM_USER", "radioss113")


def parse_cards():
    output = subprocess.run(["arecord", "-l"], capture_output=True, text=True)
    return re.findall(r"card (\d+):", output.stdout)


def create_env_file(card, instance_name):
    content = (
        f"CARD={card}\n"
        f"INSTANCE={instance_name}\n"
        f"SERVER={SERVER}\n"
        f"PORT={STREAM_PORT}\n"
        f"PASSWORD={PASSWORD}\n"
        f"BITRATE={BITRATE}\n"
    )
    env_path = INST_DIR / f"{instance_name}.env"
    with open(env_path, "w") as f:
        f.write(content)
    os.chmod(env_path, 0o644)
    return env_path


def create_recording_dir(instance):
    rec_path = REC_DIR / instance
    rec_path.mkdir(parents=True, exist_ok=True)
    shutil.chown(rec_path, user=STREAM_USER, group=STREAM_USER)


def is_service_active(instance):
    result = subprocess.run(
        ["systemctl", "is-active", f"ffmpeg-stream@{instance}.service"],
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() == "active"


def check_mountpoint(base_url, mount):
    url = f"{base_url}/{mount}"
    try:
        r = requests.get(url, timeout=3, stream=True)
        r.close()
        return r.status_code == 200, url
    except requests.RequestException:
        return False, url


def main():
    print("[stream-discover] Fermo i servizi FFmpeg esistenti...")
    subprocess.run("systemctl stop ffmpeg-stream@mic*", shell=True, check=False)

    print("[stream-discover] Pulizia dei vecchi file .env...")
    INST_DIR.mkdir(parents=True, exist_ok=True)
    for env_file in INST_DIR.glob("mic*.env"):
        env_file.unlink()

    cards = parse_cards()

    print("[stream-discover] Ricarico systemd...")
    subprocess.run(["systemctl", "daemon-reload"], check=True)

    instances = []
    for i, card in enumerate(cards):
        instance = f"mic{i}"
        print(f"[stream-discover] Configuro {instance} (card {card})...")
        create_recording_dir(instance)
        create_env_file(card, instance)
        instances.append(instance)

    service_units = [f"ffmpeg-stream@{inst}.service" for inst in instances]
    print(f"[stream-discover] Avvio simultaneo dei servizi: {', '.join(service_units)}")
    subprocess.run(["systemctl", "restart"] + service_units, check=False)

    print("[stream-discover] Attendo 5 secondi per la negoziazione...")
    time.sleep(5)

    for inst in instances:
        if is_service_active(inst):
            print(f"[stream-discover] ffmpeg-stream@{inst} attivo")
            ok, url = check_mountpoint(CHECK_BASE_URL, f"{inst}.opus")
            if ok:
                print(f"[stream-discover] mountpoint verificato e attivo: {url}")
            else:
                print(f"[stream-discover] Attenzione: {url} non risponde al test HTTP")
        else:
            print(f"[stream-discover] Errore: ffmpeg-stream@{inst} non si e avviato")


if __name__ == "__main__":
    main()
