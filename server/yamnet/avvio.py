#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import csv
import datetime
import faulthandler
import json
import logging
import os
import re
import signal
import subprocess
import sys
import time
from collections import Counter, defaultdict
from logging.handlers import RotatingFileHandler
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

import numpy as np
import soundfile as sf
import tensorflow as tf
import tensorflow_hub as hub

os.environ["CUDA_VISIBLE_DEVICES"] = "-1"
os.environ["TF_CPP_MIN_LOG_LEVEL"] = "3"
os.environ["TF_ENABLE_ONEDNN_OPTS"] = "0"
os.environ.setdefault("TFHUB_CACHE_DIR", "/var/tmp/yamnet/tfhub")
os.environ.setdefault("OMP_NUM_THREADS", "1")

try:
    tf.config.set_visible_devices([], "GPU")
except Exception:
    pass
try:
    tf.config.optimizer.set_jit(False)
except Exception:
    pass

LOG_DIR = Path("/var/tmp/yamnet")
LOG_DIR.mkdir(parents=True, exist_ok=True)
LOG_PATH = LOG_DIR / "yamnet.log"

logger = logging.getLogger("yamnet")
logger.setLevel(logging.INFO)
fmt = logging.Formatter("%(asctime)s %(levelname)s: %(message)s")
fh = RotatingFileHandler(LOG_PATH, maxBytes=5 * 1024 * 1024, backupCount=2)
fh.setFormatter(fmt)
logger.addHandler(fh)
sh = logging.StreamHandler(sys.stderr)
sh.setFormatter(fmt)
logger.addHandler(sh)
logger.propagate = False

try:
    faulthandler.enable(file=open(LOG_PATH, "a"), all_threads=True)
except Exception:
    pass

BASE_DIR = Path(__file__).resolve().parent
MODEL_HANDLE = "https://tfhub.dev/google/yamnet/1"
ASSETS_DIR = BASE_DIR / "assets"
ASSETS_DIR.mkdir(parents=True, exist_ok=True)
CLASS_MAP_PATH = ASSETS_DIR / "yamnet_class_map.csv"
CLASS_MAP_URL = "https://raw.githubusercontent.com/tensorflow/models/master/research/audioset/yamnet/yamnet_class_map.csv"

STREAM_URL = os.getenv("YAMNET_SOURCE_URL", "http://127.0.0.1:8001/mic1.opus")
FFMPEG_BIN = os.getenv("FFMPEG", "/usr/bin/ffmpeg")
DURATION = int(os.getenv("YAMNET_BLOCK_SECONDS", "60"))
OVERLAP_S = float(os.getenv("YAMNET_OVERLAP_SEC", "1.5"))
WATCHDOG_IDLE_S = max(10, DURATION * 2 + 5)
SR = 16000
BYTES_PER_SAMPLE = 2

TARGET_CLASSES = ["Car", "Truck", "Motorcycle", "Dog", "Music"]
PATCH_HOP_SECONDS = 0.48
SMOOTH_WIN = 3
MIN_PATCHES_ON = {"Car": 2, "Truck": 2, "Motorcycle": 2, "Dog": 2, "Music": 2}
THRESHOLDS = {"Car": 0.18, "Truck": 0.08, "Motorcycle": 0.01, "Dog": 0.30, "Music": 0.40}
REFRACTORY_S = {"Car": 1.2, "Truck": 2.5, "Motorcycle": 2.0, "Dog": 8.0, "Music": 5.0}
DEBUG_CLASSES = True
TOPK_DEBUG = 5

CSV_LOG = Path.home() / "vehicle_counts.csv"
EVENTS_CSV = Path("/var/www/html/web/yn/yamnet_events.csv")
CSV_LOG.parent.mkdir(parents=True, exist_ok=True)
EVENTS_CSV.parent.mkdir(parents=True, exist_ok=True)

EVENTS_PUSH_URL = os.getenv("EVENTS_PUSH_URL", "http://127.0.0.1:8090/push")
EVENTS_PUSH_TOKEN = os.getenv("EVENTS_PUSH_TOKEN", "CHANGE_ME")
MIC_NAME_ENV = os.getenv("MIC_NAME", None)


def _infer_mic_name(url: str) -> str:
    m = re.search(r"/(mic[0-9])(?:\.|/|$)", url)
    return m.group(1) if m else "mic?"


MIC_NAME = MIC_NAME_ENV or _infer_mic_name(STREAM_URL)


def dt_local_to_epoch_ms(dt: datetime.datetime) -> int:
    return int(time.mktime(dt.timetuple()) * 1000 + dt.microsecond / 1000)


def push_detection(mic: str, ts_dt: datetime.datetime, model: str, label: str, confidence: float | None = None) -> None:
    payload = {
        "mic": mic,
        "ts_utc": dt_local_to_epoch_ms(ts_dt),
        "model": model,
        "label": label,
        "confidence": None if confidence is None else float(confidence),
    }
    data = json.dumps(payload).encode("utf-8")
    req = Request(
        EVENTS_PUSH_URL,
        data=data,
        headers={"Content-Type": "application/json", "x-auth-token": EVENTS_PUSH_TOKEN},
        method="POST",
    )
    try:
        with urlopen(req, timeout=3) as r:
            if getattr(r, "status", 200) != 200:
                logger.warning("push_detection: HTTP %s %s", r.status, getattr(r, "reason", ""))
    except (HTTPError, URLError, TimeoutError) as e:
        logger.warning("push_detection fallito: %s", e)
    except Exception:
        logger.exception("push_detection: eccezione imprevista")


def _download_class_map(dst: Path) -> None:
    logger.info("Scarico class map -> %s", dst)
    with urlopen(CLASS_MAP_URL, timeout=15) as r:
        data = r.read()
    dst.write_bytes(data)


def load_class_names(csv_path: Path) -> list[str]:
    if (not csv_path.exists()) or (csv_path.stat().st_size < 10):
        _download_class_map(csv_path)
    names = []
    with csv_path.open("r", newline="", encoding="utf-8", errors="ignore") as f:
        reader = csv.reader(f, delimiter=",")
        _ = next(reader, None)
        for row in reader:
            if len(row) >= 3:
                names.append(row[2].strip())
    if len(names) < 500:
        raise ValueError(f"Class map {csv_path} non valida (labels lette: {len(names)})")
    return names


_pcm_proc: subprocess.Popen | None = None
_state_last_event_time = {cls: None for cls in TARGET_CLASSES}


def start_pcm_reader() -> subprocess.Popen:
    if not Path(FFMPEG_BIN).exists():
        raise RuntimeError(f"ffmpeg non trovato a {FFMPEG_BIN}")
    cmd = [
        FFMPEG_BIN, "-nostdin", "-hide_banner", "-loglevel", "warning",
        "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5",
        "-fflags", "+genpts", "-use_wallclock_as_timestamps", "1",
        "-i", STREAM_URL, "-map", "0:a:0", "-vn", "-sn", "-dn",
        "-acodec", "pcm_s16le", "-ar", str(SR), "-ac", "1", "-f", "s16le", "pipe:1",
    ]
    logger.info("Avvio ffmpeg PCM pipe...")
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)


def smooth(x: np.ndarray, win: int) -> np.ndarray:
    if win <= 1:
        return x
    return np.convolve(x, np.ones(win) / win, mode="same")


def debug_block(scores_np: np.ndarray, class_names: list[str]) -> None:
    if not DEBUG_CLASSES:
        return
    try:
        max_per_target = {}
        for cls in TARGET_CLASSES:
            if cls in class_names:
                idx = class_names.index(cls)
                max_per_target[cls] = float(scores_np[:, idx].max())
        logger.info("DEBUG max score target: " + ", ".join(f"{k}={v:.3f}" for k, v in max_per_target.items()))
        topk = np.argpartition(-scores_np, TOPK_DEBUG - 1, axis=1)[:, :TOPK_DEBUG]
        row_idx = np.arange(topk.shape[0])[:, None]
        topk_sorted = np.argsort(-scores_np[row_idx, topk], axis=1)
        topk = topk[row_idx, topk_sorted]
        labels = [class_names[int(i)] for i in topk.flatten()]
        common = ", ".join([f"{lab}x{cnt}" for lab, cnt in Counter(labels).most_common(3)])
        logger.info("DEBUG top-%s piu frequenti: %s", TOPK_DEBUG, common)
    except Exception:
        logger.exception("DEBUG_CLASSES failure")


def detect_events_stateful(block_start, scores_np, class_names, last_event_time):
    name_to_index = {n: i for i, n in enumerate(class_names)}
    events = {cls: [] for cls in TARGET_CLASSES}
    for cls in TARGET_CLASSES:
        idx = name_to_index.get(cls, None)
        if idx is None:
            continue
        scores = scores_np[:, idx]
        smoothed = smooth(scores, SMOOTH_WIN)
        active = smoothed >= THRESHOLDS[cls]
        on_count = 0
        for i, flag in enumerate(active):
            on_count = on_count + 1 if flag else 0
            if on_count == MIN_PATCHES_ON.get(cls, 2):
                ts = block_start + datetime.timedelta(seconds=i * PATCH_HOP_SECONDS)
                last_ts = last_event_time.get(cls)
                if (last_ts is None) or ((ts - last_ts).total_seconds() >= REFRACTORY_S[cls]):
                    events[cls].append(ts)
                    last_event_time[cls] = ts
    counts = {cls: len(events[cls]) for cls in TARGET_CLASSES}
    return counts, events, last_event_time


def log_counts_to_csv(date_str: str, counts: dict[str, int]) -> None:
    existing = defaultdict(int)
    if CSV_LOG.exists():
        with CSV_LOG.open("r", newline="") as f:
            reader = csv.reader(f)
            _ = next(reader, None)
            for row in reader:
                if len(row) >= 3 and row[0] == date_str:
                    existing[row[1]] = int(row[2])
    for cls, count in counts.items():
        existing[cls] += count
    with CSV_LOG.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["Date", "Vehicle Type", "Count"])
        for cls in TARGET_CLASSES:
            writer.writerow([date_str, cls, existing.get(cls, 0)])


def _log_events_to_csv(ev_map):
    new_file = not EVENTS_CSV.exists()
    with EVENTS_CSV.open("a", newline="") as f:
        writer = csv.writer(f)
        if new_file:
            writer.writerow(["timestamp", "label"])
        for cls, times in ev_map.items():
            for ts in times:
                writer.writerow([ts.strftime("%Y-%m-%dT%H:%M:%S"), cls])


def analyze_block(block_f32: np.ndarray, block_start_dt: datetime.datetime, model, class_names: list[str]) -> None:
    global _state_last_event_time
    scores, _, _ = model(block_f32)
    scores_np = scores.numpy()
    debug_block(scores_np, class_names)
    counts, ev, _state_last_event_time = detect_events_stateful(block_start_dt, scores_np, class_names, _state_last_event_time)
    ev_for_push = {cls: [ts for ts in times if ts >= block_start_dt] for cls, times in ev.items()}
    name_to_idx = {n: i for i, n in enumerate(class_names)}
    per_class_conf = {}
    for cls in TARGET_CLASSES:
        idx = name_to_idx.get(cls)
        per_class_conf[cls] = float(scores_np[:, idx].max()) if idx is not None else None
    for cls, times in ev_for_push.items():
        conf = per_class_conf.get(cls)
        for ts_dt in times:
            push_detection(MIC_NAME, ts_dt, model="yamnet", label=cls, confidence=conf)
    _log_events_to_csv(ev_for_push)
    date_str = block_start_dt.strftime("%Y-%m-%d")
    log_counts_to_csv(date_str, {cls: len(ev_for_push.get(cls, [])) for cls in TARGET_CLASSES})


def continuous_stream_loop(model, class_names):
    global _pcm_proc
    block_samples = int(max(1, DURATION) * SR)
    overlap_samples = int(max(0.0, OVERLAP_S) * SR)
    hop_samples = max(1, block_samples - overlap_samples)
    block_bytes = block_samples * BYTES_PER_SAMPLE
    hop_bytes = hop_samples * BYTES_PER_SAMPLE
    buf = bytearray()
    t0 = None
    total_emitted_samples = 0
    last_byte_ts = time.time()
    chunk_size = 8192

    while True:
        try:
            if _pcm_proc is None or _pcm_proc.poll() is not None:
                _pcm_proc = start_pcm_reader()
                buf.clear()
                t0 = None
                total_emitted_samples = 0
                last_byte_ts = time.time()

            chunk = _pcm_proc.stdout.read(chunk_size)
            if not chunk:
                if _pcm_proc.poll() is not None:
                    logger.warning("ffmpeg terminato, riavvio...")
                    try:
                        _pcm_proc.kill()
                    except Exception:
                        pass
                    _pcm_proc = None
                    time.sleep(0.5)
                    continue
                if (time.time() - last_byte_ts) > WATCHDOG_IDLE_S:
                    logger.warning("Nessun dato dalla pipe per troppo tempo, riavvio ffmpeg...")
                    try:
                        _pcm_proc.terminate()
                        try:
                            _pcm_proc.wait(timeout=2)
                        except Exception:
                            _pcm_proc.kill()
                    except Exception:
                        pass
                    _pcm_proc = None
                else:
                    time.sleep(0.02)
                continue

            last_byte_ts = time.time()
            buf += chunk
            if t0 is None:
                t0 = datetime.datetime.now()

            while len(buf) >= block_bytes:
                block_raw = memoryview(buf)[:block_bytes]
                block_start_dt = t0 + datetime.timedelta(seconds=total_emitted_samples / SR)
                block_i16 = np.frombuffer(block_raw, dtype=np.int16)
                block_f32 = (block_i16.astype(np.float32)) / 32768.0
                analyze_block(block_f32, block_start_dt, model, class_names)
                buf = bytearray(buf[hop_bytes:])
                total_emitted_samples += hop_samples

        except Exception:
            logger.exception("Errore nel loop di streaming continuo; riavvio ffmpeg...")
            try:
                if _pcm_proc and _pcm_proc.poll() is None:
                    _pcm_proc.terminate()
                    try:
                        _pcm_proc.wait(timeout=2)
                    except Exception:
                        _pcm_proc.kill()
            except Exception:
                pass
            _pcm_proc = None
            time.sleep(0.5)


def _shutdown(signum=None, frame=None):
    global _pcm_proc
    logger.info("Shutdown richiesto, chiudo ffmpeg...")
    try:
        if _pcm_proc and _pcm_proc.poll() is None:
            _pcm_proc.terminate()
            try:
                _pcm_proc.wait(timeout=3)
            except Exception:
                _pcm_proc.kill()
    except Exception:
        pass
    sys.exit(0)


signal.signal(signal.SIGINT, _shutdown)
signal.signal(signal.SIGTERM, _shutdown)


def main():
    logger.info("Carico YAMNet da TF-Hub...")
    try:
        model = hub.load(MODEL_HANDLE)
    except Exception as e:
        logger.exception("Errore caricando YAMNet: %s", e)
        time.sleep(3)
        return

    try:
        class_names = load_class_names(CLASS_MAP_PATH)
    except Exception as e:
        logger.exception("Errore caricando class map: %s", e)
        time.sleep(3)
        return

    missing = [c for c in TARGET_CLASSES if c not in class_names]
    if missing:
        logger.warning("ATTENZIONE: label non trovate nella class-map: %s", missing)

    logger.info("Config: DURATION=%ss, OVERLAP_S=%s, SR=%s, STREAM_URL=%s", DURATION, OVERLAP_S, SR, STREAM_URL)
    continuous_stream_loop(model, class_names)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        logger.exception("CRASH top-level")
        raise
