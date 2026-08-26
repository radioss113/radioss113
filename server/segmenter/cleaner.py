#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import shutil
from pathlib import Path
from datetime import datetime

DAYS_TO_KEEP = 3
RECORDINGS_ROOT = Path("/var/www/html/web/recordings")


def cleanup_old_days(days_to_keep: int = DAYS_TO_KEEP):
    if not RECORDINGS_ROOT.exists():
        return

    valid_dirs = []
    for d in RECORDINGS_ROOT.iterdir():
        if d.is_dir():
            try:
                datetime.strptime(d.name, "%Y-%m-%d")
                valid_dirs.append(d)
            except ValueError:
                continue

    valid_dirs.sort(key=lambda p: p.name)

    keep_count = days_to_keep + 1
    if len(valid_dirs) > keep_count:
        to_delete = valid_dirs[:-keep_count]
        for d in to_delete:
            try:
                shutil.rmtree(d)
                print(f"[CLEANUP] Rimossa cartella {d}")
            except Exception as e:
                print(f"[CLEANUP-ERROR] {d}: {e}")


if __name__ == "__main__":
    print("[CLEANER] Avvio test standalone...")
    cleanup_old_days(days_to_keep=2)
    print("[CLEANER] Test completato.")
