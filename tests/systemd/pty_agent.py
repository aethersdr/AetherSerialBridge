#!/usr/bin/env python3
"""A real polkit auth_admin prompt without a desktop (v1.1 WS-15): pkttyagent, the agent a terminal session uses, on a
pseudo-terminal this script drives, registered for the launcher's pid (pkexec asks polkit about its PARENT, the
launcher). Runs as the requesting user inside the container.

usage: pty_agent.py --answer <password> | --eof  [--timeout S]  -- <asb_stage_client args... --gate>

The launcher (asb_stage_client --gate) prints {"gate":<pid>} and waits; pkttyagent --process <pid> --notify-fd
registers; then the gate opens and pkexec starts. Each "Password:" prompt is answered (or the terminal sends EOF) only
once pkttyagent has turned the terminal's echo off: it flushes typed-ahead input when it does (TCSAFLUSH), so an answer
written earlier is lost. Prints "PROMPTS <n>", the transcript's last lines and the launcher's JSON line, last."""

import json
import os
import pty
import select
import subprocess
import sys
import termios
import time


def main():
    argv = sys.argv[1:]
    sep = argv.index("--")
    opts, launcher_args = argv[:sep], argv[sep + 1:]
    answer = None
    timeout = 120.0
    i = 0
    while i < len(opts):
        if opts[i] == "--answer":
            answer = opts[i + 1].encode() + b"\n"
            i += 2
        elif opts[i] == "--eof":
            answer = b"\x04"
            i += 1
        elif opts[i] == "--timeout":
            timeout = float(opts[i + 1])
            i += 2
        else:
            sys.exit("unknown option %s" % opts[i])
    if answer is None:
        sys.exit("--answer or --eof")

    launcher = subprocess.Popen(launcher_args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    gate = json.loads(launcher.stdout.readline())
    pid_to_watch = int(gate["gate"])
    notify_r, notify_w = os.pipe()
    child, fd = pty.fork()
    if child == 0:
        os.dup2(notify_w, 9)
        os.execvp("pkttyagent", ["pkttyagent", "--process", str(pid_to_watch), "--notify-fd", "9"])
    os.close(notify_w)
    ready, _, _ = select.select([notify_r], [], [], 20)
    if not ready:
        sys.exit("pkttyagent did not register")
    launcher.stdin.write(b"go\n")
    launcher.stdin.flush()

    transcript, prompts, end = b"", 0, time.time() + timeout
    while time.time() < end and launcher.poll() is None:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        transcript += chunk
        if transcript.count(b"Password:") > prompts:
            prompts += 1
            settle = time.time() + 10
            while time.time() < settle and termios.tcgetattr(fd)[3] & termios.ECHO:
                time.sleep(0.05)
            time.sleep(0.1)
            os.write(fd, answer)
    try:
        out, _ = launcher.communicate(timeout=max(1.0, end - time.time()))
    except subprocess.TimeoutExpired:
        launcher.kill()
        out, _ = launcher.communicate()
    try:
        os.kill(child, 9)
    except ProcessLookupError:
        pass
    print("PROMPTS %d" % prompts)
    tail = transcript.decode(errors="replace").replace("\r", "").strip().splitlines()[-6:]
    for line in tail:
        print("PTY: %s" % line)
    sys.stdout.write(out.decode(errors="replace"))
    sys.stdout.flush()


if __name__ == "__main__":
    main()
