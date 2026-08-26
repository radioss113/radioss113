#!/usr/bin/env python3

import csv
import datetime
import glob
import os
import subprocess
import sys

SOURCE_URL = os.getenv("BIRDNET_SOURCE_URL", "http://127.0.0.1:8001/mic2.opus")
SAVE_DIR = os.getenv("BIRDNET_SAVE_DIR", "/etc/birdnet/tmp")
OUTPUT_DIR = os.path.expanduser(os.getenv("BIRDNET_OUTPUT_DIR", "/etc/birdnet/results"))
COMBINED_CSV = os.path.join(OUTPUT_DIR, "all_results.csv")
LAT = os.getenv("BIRDNET_LAT", "38.0")
LON = os.getenv("BIRDNET_LON", "13.0")
EVENTS_CSV = os.getenv("BIRDNET_EVENTS_CSV", "/var/www/html/web/bn/birdnet_events.csv")
PYTHON_BIN = os.getenv("PYTHON_BIN", sys.executable)

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(os.path.dirname(EVENTS_CSV), exist_ok=True)
os.makedirs(SAVE_DIR, exist_ok=True)


def get_week_number():
    return str(datetime.datetime.now().isocalendar().week)


def download_audio():
    now = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    filename = f"audio_{now}.wav"
    filepath = os.path.join(SAVE_DIR, filename)
    try:
        subprocess.run(
            [
                "ffmpeg", "-y", "-i", SOURCE_URL,
                "-t", "60", "-acodec", "pcm_s16le", "-ar", "48000", "-ac", "1", filepath,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return filepath if os.path.exists(filepath) else None
    except Exception as e:
        print(f"[!] Errore durante la registrazione: {e}")
        return None


def append_to_combined(rows, header):
    write_header = not os.path.exists(COMBINED_CSV)
    with open(COMBINED_CSV, "a", newline="") as dst:
        writer = csv.writer(dst, delimiter="\t")
        if write_header:
            writer.writerow(header)
        writer.writerows(rows)


def append_to_events(rows):
    write_header = not os.path.exists(EVENTS_CSV)
    with open(EVENTS_CSV, "a", newline="") as f:
        writer = csv.writer(f)
        if write_header:
            writer.writerow(["timestamp", "label"])
        now_iso = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
        for r in rows:
            if len(r) > 8:
                species = r[8]
                if species and species.lower() != "nocall":
                    writer.writerow([now_iso, species])


def analyze_audio(filepath):
    timestamp = datetime.datetime.now().strftime("[%Y-%m-%d %H:%M:%S]")

    for f in glob.glob(os.path.join(OUTPUT_DIR, "*.BirdNET.selection.table.txt")):
        os.remove(f)

    try:
        subprocess.run(
            [
                PYTHON_BIN, "-m", "birdnet_analyzer.analyze",
                "-o", OUTPUT_DIR,
                "--lat", LAT,
                "--lon", LON,
                "--week", get_week_number(),
                filepath,
            ],
            check=True,
        )

        result_files = glob.glob(os.path.join(OUTPUT_DIR, "*.BirdNET.selection.table.txt"))
        if not result_files:
            print(f"{timestamp} • Nessun file di output trovato")
            return

        result_file = result_files[0]
        with open(result_file, newline="") as f:
            reader = csv.reader(f, delimiter="\t")
            rows = list(reader)

        if len(rows) <= 1:
            print(f"{timestamp} • Nessun risultato per {filepath}")
            return

        header, data_rows = rows[0], rows[1:]
        filtered_rows = [
            r for r in data_rows
            if len(r) > 10 and os.path.exists(r[10]) and os.path.samefile(r[10], filepath) and r[8].lower() != "nocall"
        ]

        if filtered_rows:
            print(f"{timestamp} ✓ {len(filtered_rows)} detections salvate dal file {filepath}")
            append_to_combined(filtered_rows, header)
            append_to_events(filtered_rows)
        else:
            print(f"{timestamp} • Solo nocall o nessuna detection valida per {filepath}")

    except subprocess.CalledProcessError:
        print(f"{timestamp} [!] Errore durante l'analisi del file {filepath}")
    finally:
        if os.path.exists(filepath):
            os.remove(filepath)


while True:
    print("Registrazione audio in corso...")
    audio_file = download_audio()
    if audio_file:
        print(f"Registrato: {audio_file}")
        analyze_audio(audio_file)
