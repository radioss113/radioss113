#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import logging
import os
import re
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, Optional, Set
from urllib.parse import urlparse

import requests
import sseclient
from zoneinfo import ZoneInfo

from cleaner import cleanup_old_days

TZ = ZoneInfo("Europe/Rome")
LOG_FORMAT = "[%(asctime)s] %(levelname)s: %(message)s"
logging.basicConfig(level=logging.INFO, format=LOG_FORMAT)
log = logging.getLogger("icecast-segmenter")

RECORDINGS_ROOT = Path(os.getenv("RECORDINGS_ROOT", "/var/www/html/web/recordings"))
TIMESTAMP_PUSH_URL = os.getenv("TIMESTAMP_PUSH_URL", "http://127.0.0.1:8090/push")
TIMESTAMP_TOKEN = os.getenv("EVENTS_PUSH_TOKEN", "CHANGE_ME")
PUBLIC_BASE_URL = os.getenv("PUBLIC_BASE_URL", "https://radioss113.it/recordings")

RUNNING = True


def _handle_sigterm(signum, frame):
    global RUNNING
    log.info("Segnale %s ricevuto: arresto in corso...", signum)
    RUNNING = False


for _sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(_sig, _handle_sigterm)


def next_top_of_hour_local(now: datetime) -> datetime:
    return now.replace(minute=0, second=0, microsecond=0) + timedelta(hours=1)


def hour_floor_local(ts: datetime) -> datetime:
    return ts.replace(minute=0, second=0, microsecond=0)


CONTENT_TYPE_EXT = {
    "audio/ogg": ".ogg",
    "application/ogg": ".ogg",
    "audio/opus": ".ogg",
    "audio/vorbis": ".ogg",
    "audio/mpeg": ".mp3",
    "audio/aac": ".aac",
    "audio/aacp": ".aac",
    "audio/flac": ".flac",
}
OGG_LIKE_CT = {"audio/ogg", "application/ogg", "audio/opus", "audio/vorbis"}


def ensure_dir(path: Path):
    path.mkdir(parents=True, exist_ok=True)


def guess_ct_and_ext(resp: requests.Response, url: str) -> tuple[str, str]:
    ct = (resp.headers.get("Content-Type") or "").split(";")[0].strip().lower()
    ext = CONTENT_TYPE_EXT.get(ct)
    if not ext:
        ext = Path(urlparse(url).path).suffix.lower() or ".bin"
    return ct, ext


def mount_name_from_url(url: str) -> str:
    name = Path(urlparse(url).path).name
    if not name:
        return "stream"
    return name.split(".")[0]


def open_new_segment_file(ts_start_local: datetime, mount: str, ext: str) -> Path:
    day_dir = RECORDINGS_ROOT / ts_start_local.strftime("%Y-%m-%d") / mount
    ensure_dir(day_dir)
    cleanup_old_days(days_to_keep=2)
    fname = f"{ts_start_local.strftime('%Y-%m-%dT%H-%M-%S')}_{mount}{ext}.part"
    return day_dir / fname


def finalize_segment_file(tmp_path: Path) -> Path:
    final_path = tmp_path.with_suffix("")
    tmp_path.rename(final_path)
    return final_path


def public_url_for(file_path: Path) -> str:
    rel_path = file_path.relative_to(RECORDINGS_ROOT)
    return f"{PUBLIC_BASE_URL}/{rel_path.as_posix()}"


def push_segment_event(mount: str, file_path: Path):
    public_url = public_url_for(file_path)
    ev = {
        "ts_utc": int(time.time() * 1000),
        "mic": mount,
        "model": "segmenter",
        "label": "file_ready",
        "confidence": 1.0,
        "url": public_url,
    }
    try:
        r = requests.post(
            TIMESTAMP_PUSH_URL,
            headers={"x-auth-token": TIMESTAMP_TOKEN},
            json=ev,
            timeout=5,
        )
        r.raise_for_status()
        print(f"[PUSH] Nuovo file pronto: {public_url}")
    except Exception as e:
        print(f"[ERROR] Push evento segmenter: {e}")


@dataclass
class FFmpegSegmentWriter:
    url: str
    mount: str
    process: Optional[subprocess.Popen] = None
    last_opened_file: Optional[Path] = None
    last_closed_file: Optional[Path] = None

    def start(self):
        out_pattern = str(RECORDINGS_ROOT / "%Y-%m-%d" / self.mount / f"%Y-%m-%dT%H-%M-%S_{self.mount}.ogg")
        cmd = [
            "ffmpeg", "-hide_banner", "-nostdin", "-y",
            "-user_agent", "RadioSS113-Archiver/1.0",
            "-i", self.url,
            "-c", "copy", "-map", "0:a",
            "-f", "segment",
            "-segment_time", "3600",
            "-segment_atclocktime", "1",
            "-strftime", "1",
            "-reset_timestamps", "1",
            out_pattern,
        ]
        log.info("[%s] Avvio ffmpeg (segmentazione oraria con header Ogg/Opus)", self.mount)
        self.process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._read_log, daemon=True).start()

    def _read_log(self):
        opening_re = re.compile(r"Opening '([^']+)' for writing")
        for line in self.process.stdout:
            m = opening_re.search(line)
            if not m:
                continue
            opened = Path(m.group(1))
            ensure_dir(opened.parent)
            cleanup_old_days(days_to_keep=2)

            if self.last_opened_file and self.last_opened_file != opened:
                self.last_closed_file = self.last_opened_file
                try:
                    print(f"[READY] {self.last_closed_file}")
                    # push_segment_event(self.mount, self.last_closed_file)
                except Exception:
                    pass
            self.last_opened_file = opened
            print(f"[OPEN]  {opened}")

        if self.last_opened_file:
            try:
                print(f"[READY] {self.last_opened_file}")
                # push_segment_event(self.mount, self.last_opened_file)
            except Exception:
                pass

    def stop(self):
        if self.process and self.process.poll() is None:
            log.info("[%s] Stop ffmpeg", self.mount)
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()


@dataclass
class RawSegmentWriter:
    url: str
    mount: str
    ct: str
    ext: str
    reconnect_wait: int
    chunk_size: int

    def run(self):
        headers = {"User-Agent": "RadioSS113-Segmenter/1.0", "Icy-MetaData": "0"}
        session = requests.Session()
        current_file: Optional[Path] = None
        current_fp = None
        segment_end_local: Optional[datetime] = None

        while RUNNING:
            try:
                with session.get(self.url, headers=headers, stream=True, timeout=(5, 15)) as resp:
                    resp.raise_for_status()
                    now = datetime.now(TZ)
                    seg_start_local = now
                    segment_end_local = next_top_of_hour_local(now)

                    start_for_name = hour_floor_local(seg_start_local) if (
                        seg_start_local.minute == 0 and seg_start_local.second == 0 and seg_start_local.microsecond == 0
                    ) else seg_start_local

                    current_file = open_new_segment_file(start_for_name, self.mount, self.ext)
                    current_fp = open(current_file, "wb")
                    print(f"[NEW FILE] {current_file} fino a {segment_end_local.isoformat()}")

                    for chunk in resp.iter_content(chunk_size=self.chunk_size):
                        if not RUNNING:
                            break
                        if not chunk:
                            continue
                        current_fp.write(chunk)

                        now = datetime.now(TZ)
                        if now >= segment_end_local:
                            try:
                                if current_fp and not current_fp.closed:
                                    current_fp.flush()
                                    os.fsync(current_fp.fileno())
                                    current_fp.close()
                                finalized = finalize_segment_file(current_file)
                                print(f"[FINALIZZA] {finalized}")
                                # push_segment_event(self.mount, finalized)
                            except Exception as e:
                                log.exception("[%s] Errore finalizzazione: %s", self.mount, e)

                            seg_start_local = hour_floor_local(segment_end_local)
                            segment_end_local = seg_start_local + timedelta(hours=1)
                            current_file = open_new_segment_file(seg_start_local, self.mount, self.ext)
                            current_fp = open(current_file, "wb")
                            print(f"[NEW FILE] {current_file} fino a {segment_end_local.isoformat()}")

                try:
                    if current_fp and not current_fp.closed:
                        current_fp.flush()
                        os.fsync(current_fp.fileno())
                        current_fp.close()
                    if current_file and current_file.suffix.endswith(".part"):
                        finalized = finalize_segment_file(current_file)
                        print(f"[FINALIZZA] {finalized}")
                        # push_segment_event(self.mount, finalized)
                except Exception:
                    pass

                time.sleep(self.reconnect_wait)

            except Exception as e:
                print(f"[ERROR] Mount={self.mount} eccezione={e}")
                try:
                    if current_fp and not current_fp.closed:
                        current_fp.flush()
                        os.fsync(current_fp.fileno())
                        current_fp.close()
                    if current_file and current_file.suffix.endswith(".part"):
                        finalized = finalize_segment_file(current_file)
                        print(f"[FINALIZZA] {finalized}")
                        # push_segment_event(self.mount, finalized)
                except Exception:
                    pass
                time.sleep(self.reconnect_wait)


@dataclass
class SegmenterConfig:
    url: str
    reconnect_wait: int = 5
    chunk_size: int = 16384


class Segmenter(threading.Thread):
    def __init__(self, cfg: SegmenterConfig):
        super().__init__(daemon=True)
        self.cfg = cfg
        self.stop_event = threading.Event()
        self._session = requests.Session()
        self._mount = mount_name_from_url(cfg.url)
        self._ff: Optional[FFmpegSegmentWriter] = None
        self._raw: Optional[RawSegmentWriter] = None

    def stop(self):
        self.stop_event.set()
        if self._ff:
            self._ff.stop()

    def run(self):
        print(f"[AVVIO] Segmenter mount={self._mount} url={self.cfg.url}")
        headers = {"User-Agent": "RadioSS113-Segmenter/1.0", "Icy-MetaData": "0"}

        while RUNNING and not self.stop_event.is_set():
            try:
                r = self._session.get(self.cfg.url, headers=headers, stream=True, timeout=(5, 15))
                r.raise_for_status()
                ct, ext = guess_ct_and_ext(r, self.cfg.url)
                r.close()
                print(f"[INFO] Mount={self._mount} Content-Type={ct} -> ext={ext}")

                if ct in OGG_LIKE_CT or ext == ".ogg":
                    self._ff = FFmpegSegmentWriter(self.cfg.url, self._mount)
                    self._ff.start()
                    while RUNNING and not self.stop_event.is_set():
                        if self._ff.process and self._ff.process.poll() is not None:
                            print(f"[WARN] ffmpeg terminato per {self._mount}, retry fra {self.cfg.reconnect_wait}s")
                            time.sleep(self.cfg.reconnect_wait)
                            self._ff.start()
                        time.sleep(1)
                else:
                    self._raw = RawSegmentWriter(
                        self.cfg.url, self._mount, ct, ext, self.cfg.reconnect_wait, self.cfg.chunk_size
                    )
                    self._raw.run()

            except Exception as e:
                print(f"[ERROR] Mount={self._mount} eccezione={e}")
                time.sleep(self.cfg.reconnect_wait)

        if self._ff:
            self._ff.stop()
        print(f"[STOP] Segmenter mount={self._mount}")


@dataclass
class ManagerConfig:
    status_url: str
    events_url: str
    reconnect_wait: int = 5


class Manager:
    def __init__(self, cfg: ManagerConfig):
        self.cfg = cfg
        self._session = requests.Session()
        self._segmenters: Dict[str, Segmenter] = {}
        self._lock = threading.Lock()

    def _discover_mounts_once(self) -> Set[str]:
        r = self._session.get(self.cfg.status_url, timeout=10)
        r.raise_for_status()
        data = r.json()
        icestats = data.get("icestats", {})
        src = icestats.get("source")
        mounts: Set[str] = set()
        if isinstance(src, list):
            for s in src:
                name = Path(s.get("listenurl") or "").name
                if name:
                    mounts.add(name)
        elif isinstance(src, dict):
            name = Path(src.get("listenurl") or "").name
            if name:
                mounts.add(name)
        return mounts

    def _start_segmenter(self, mount: str, source: str = "event"):
        url = f"http://127.0.0.1:8001/{mount}"
        with self._lock:
            if url in self._segmenters:
                return
            seg = Segmenter(SegmenterConfig(url=url, reconnect_wait=self.cfg.reconnect_wait))
            self._segmenters[url] = seg
            seg.start()
            print(f"[START-{source.upper()}] Nuovo segmenter mount={mount} url={url}")

    def _stop_segmenter(self, mount: str):
        url = f"http://127.0.0.1:8001/{mount}"
        with self._lock:
            seg = self._segmenters.pop(url, None)
        if seg:
            seg.stop()
            seg.join(timeout=3)
            print(f"[STOP] Segmenter fermato mount={mount}")

    def run(self):
        print(f"[MANAGER] Avvio, discovery iniziale su {self.cfg.status_url}")
        ensure_dir(RECORDINGS_ROOT)

        try:
            mounts = self._discover_mounts_once()
            for m in mounts:
                self._start_segmenter(m, source="discovery")
        except Exception as e:
            print(f"[ERROR] Discovery iniziale: {e}")

        print(f"[MANAGER] Connessione a {self.cfg.events_url}")
        resp = requests.get(self.cfg.events_url, stream=True)
        client = sseclient.SSEClient(resp)

        for event in client.events():
            if not RUNNING:
                break
            if event.event != "detection":
                continue
            try:
                ev = json.loads(event.data)
            except Exception:
                continue
            if ev.get("model") != "monitor":
                continue

            mic = ev.get("mic")
            label = ev.get("label")
            if not mic or not label:
                continue

            if label == "stream_up":
                self._start_segmenter(mic, source="event")
            elif label == "stream_down":
                self._stop_segmenter(mic)

        print("[MANAGER] Arresto: stop di tutti i segmenter")
        with self._lock:
            urls = list(self._segmenters.keys())
        for url in urls:
            self._stop_segmenter(Path(url).name)
        print("[MANAGER] Terminato")


def main():
    ap = argparse.ArgumentParser(description="Icecast segmenter")
    ap.add_argument("--status-url", default="http://127.0.0.1:8001/status-json.xsl")
    ap.add_argument("--events-url", default="http://127.0.0.1:8090/events")
    ap.add_argument("--reconnect-wait", type=int, default=5)
    args = ap.parse_args()

    mgr = Manager(
        ManagerConfig(
            status_url=args.status_url,
            events_url=args.events_url,
            reconnect_wait=args.reconnect_wait,
        )
    )
    try:
        mgr.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
