#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import json
import logging
import os
import time
from pathlib import Path
from queue import Queue

import pymysql
from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import StreamingResponse

DB_CONFIG = dict(
    host=os.getenv("DB_HOST", "127.0.0.1"),
    user=os.getenv("DB_USER", "CHANGE_ME"),
    password=os.getenv("DB_PASSWORD", "CHANGE_ME"),
    database=os.getenv("DB_NAME", "radioss113"),
    charset="utf8mb4",
    cursorclass=pymysql.cursors.DictCursor,
    autocommit=True,
)

NOMINAL_DELAY_S = 125

LOG_DIR = Path("/var/tmp/radio-timestamp")
LOG_DIR.mkdir(parents=True, exist_ok=True)
LOG_PATH = LOG_DIR / "service.log"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s: %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler(LOG_PATH, encoding="utf-8"),
    ],
)
logger = logging.getLogger("timestamp")

app = FastAPI()
event_queue = Queue()
_last_event_per_class = {}


def insert_event(ev: dict):
    try:
        ts_ms = int(ev["ts_utc"])
        sec_bucket = ts_ms // 1000
        key = (ev["mic"], ev["label"])

        last_sec = _last_event_per_class.get(key)
        if last_sec == sec_bucket:
            logger.info("Scartato duplicato %s @ %s", key, sec_bucket)
            return

        _last_event_per_class[key] = sec_bucket

        conn = pymysql.connect(**DB_CONFIG)
        with conn.cursor() as cur:
            sql = """INSERT IGNORE INTO yamnet
                     (ts_utc, mic, model, label, confidence)
                     VALUES (%s, %s, %s, %s, %s)"""
            cur.execute(
                sql,
                (
                    ts_ms,
                    ev["mic"],
                    ev["model"],
                    ev["label"],
                    ev.get("confidence"),
                ),
            )
        conn.close()
    except Exception as e:
        logger.error("DB insert_event fallito: %s", e)


@app.post("/push")
async def push_event(request: Request):
    token = request.headers.get("x-auth-token")
    if token != os.getenv("EVENTS_PUSH_TOKEN", "CHANGE_ME"):
        raise HTTPException(status_code=403, detail="Invalid token")

    ev = await request.json()
    logger.info("Ricevuto evento: %s", ev)
    insert_event(ev)
    event_queue.put({"type": "detection", "data": ev})
    return {"status": "ok"}


@app.get("/health")
async def health():
    return {
        "server_now_ms": int(time.time() * 1000),
        "delay_s": NOMINAL_DELAY_S,
    }


@app.get("/events")
async def events():
    async def event_generator():
        while True:
            hb = {
                "server_now_ms": int(time.time() * 1000),
                "delay_s": NOMINAL_DELAY_S,
            }
            yield f"event: heartbeat\ndata: {json.dumps(hb)}\n\n"

            while not event_queue.empty():
                ev = event_queue.get()
                yield f"event: {ev['type']}\ndata: {json.dumps(ev['data'])}\n\n"

            await asyncio.sleep(1)

    return StreamingResponse(event_generator(), media_type="text/event-stream")


@app.get("/recent")
def recent_events(seconds: int = Query(120, ge=1, le=600)):
    try:
        conn = pymysql.connect(**DB_CONFIG)
        with conn.cursor() as cur:
            now_ms = int(time.time() * 1000)
            cutoff_ms = now_ms - (seconds * 1000)

            cur.execute(
                """
                SELECT mic, ts_utc, model, label, confidence
                FROM yamnet
                WHERE ts_utc >= %s
                ORDER BY ts_utc ASC
                """,
                (cutoff_ms,),
            )
            rows = cur.fetchall()

        conn.close()
        return {"now_ms": now_ms, "events": rows}

    except Exception as e:
        logger.exception("Errore in /recent")
        return {"error": str(e)}
