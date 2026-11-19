"""No-window process creation for every harness subprocess (Windows).

ssh.exe, mysql.exe and python.exe are console-subsystem programs: started from a process that has
no console (a detached watcher, a GUI host, a Claude Code background job) each one allocates a NEW,
VISIBLE console window. CREATE_NO_WINDOW gives the child a hidden console instead, which its own
children inherit, and an explicit stdin keeps nothing attached to a terminal. Every Popen/run in
tools/bottest goes through popen_kw() (botperf/driver.py carries an inline copy of the flag).
"""
from __future__ import annotations

import os
import subprocess

_DEFAULT = object()


def popen_kw(stdin=_DEFAULT, new_group: bool = False) -> dict:
    """kwargs for subprocess.Popen / subprocess.run: no console window on Windows, explicit stdin.

    stdin defaults to DEVNULL; pass subprocess.PIPE for interactive sessions, or None to leave it
    out (required when subprocess.run(..., input=...) supplies stdin itself)."""
    kw: dict = {}
    if stdin is _DEFAULT:
        kw["stdin"] = subprocess.DEVNULL
    elif stdin is not None:
        kw["stdin"] = stdin
    if os.name == "nt":
        flags = subprocess.CREATE_NO_WINDOW
        if new_group:
            flags |= subprocess.CREATE_NEW_PROCESS_GROUP
        kw["creationflags"] = flags
    return kw
