#!/usr/bin/env python3
"""
pc_gui.py — Antigravity CarPlay PC GUI (Phase B.1 Live Video + Touch Screen)

Features:
- Connects to pc_main.exe H.264 video stream over TCP (127.0.0.1:18888) using OpenCV FFmpeg backend.
- Connects to pc_main.exe control port (127.0.0.1:18889) for real-time mouse -> touch mapping.
- Maps window coordinates (1024x600) to normalized touch coordinates (0..10000).
- Action 14 = Touch Down, Action 15 = Touch Move, Action 16 = Touch Up.
- Auto-starts pc_main.exe if not already running.
"""

import sys
import os
import time
import socket
import struct
import subprocess
import threading
import cv2
import numpy as np

STREAM_HOST = "127.0.0.1"
STREAM_PORT = 18888
CTRL_HOST = "127.0.0.1"
CTRL_PORT = 18889
WIN_NAME = "Antigravity CarPlay (1024x600) | Audio & Touch Enabled"
WIDTH = 1024
HEIGHT = 600

ctrl_sock = None
sock_lock = threading.Lock()
g_running = True

def send_touch(action: int, x: int, y: int):
    """Send 12-byte binary touch command: [uint32 action, uint32 x, uint32 y]"""
    global ctrl_sock
    with sock_lock:
        if ctrl_sock is None:
            return
        try:
            payload = struct.pack("<III", action, x, y)
            ctrl_sock.sendall(payload)
        except Exception as e:
            print(f"[CTRL] send error: {e}")
            ctrl_sock = None

def on_mouse(event, x, y, flags, param):
    """Translate window mouse events to normalized 0..10000 CarPlay touch events."""
    norm_x = int(max(0, min(10000, (x / float(WIDTH)) * 10000)))
    norm_y = int(max(0, min(10000, (y / float(HEIGHT)) * 10000)))

    if event == cv2.EVENT_LBUTTONDOWN:
        print(f"[TOUCH-DOWN] x={norm_x} y={norm_y} (px: {x},{y})")
        send_touch(14, norm_x, norm_y)
    elif event == cv2.EVENT_MOUSEMOVE and (flags & cv2.EVENT_FLAG_LBUTTON):
        send_touch(15, norm_x, norm_y)
    elif event == cv2.EVENT_LBUTTONUP:
        print(f"[TOUCH-UP  ] x={norm_x} y={norm_y} (px: {x},{y})")
        send_touch(16, norm_x, norm_y)

def connect_control():
    """Connect to pc_main control port in background."""
    global ctrl_sock, g_running
    while g_running:
        if ctrl_sock is None:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((CTRL_HOST, CTRL_PORT))
                s.settimeout(None)
                with sock_lock:
                    ctrl_sock = s
                print(f"[CTRL] Connected to control channel at {CTRL_HOST}:{CTRL_PORT}")
            except Exception:
                time.sleep(1.0)
        else:
            time.sleep(1.0)

def ensure_daemon():
    """Check if stream port is open, if not, spawn pc_main.exe in background."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        s.connect((STREAM_HOST, STREAM_PORT))
        s.close()
        print(f"[DAEMON] pc_main.exe is already listening on {STREAM_HOST}:{STREAM_PORT}")
        return None
    except Exception:
        pass

    # Find pc_main.exe relative to script directory
    base_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(base_dir, "dist_final", "build", "pc_main.exe"),
        os.path.join(base_dir, "..", "dist_final", "build", "pc_main.exe"),
        os.path.join(base_dir, "build", "pc_main.exe"),
        os.path.join(base_dir, "pc_main.exe"),
        r"C:\tang-carplay\dist_final\build\pc_main.exe",
    ]
    exe_path = None
    for c in candidates:
        if os.path.isfile(c):
            exe_path = os.path.abspath(c)
            break

    if not exe_path:
        print(f"[DAEMON] Warning: pc_main.exe not found in candidates: {candidates}")
        return None

    print(f"[DAEMON] Launching {exe_path}...")
    cwd = os.path.dirname(exe_path)
    log_path = os.path.join(cwd, "pc_main_daemon.log")
    log_f = open(log_path, "w", encoding="utf-8", buffering=1)
    proc = subprocess.Popen(
        [exe_path, "--stream", str(STREAM_PORT), "--ctrl", str(CTRL_PORT)],
        cwd=cwd,
        stdout=log_f,
        stderr=subprocess.STDOUT
    )
    # Wait up to 5 seconds for port to open
    for _ in range(50):
        time.sleep(0.1)
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.2)
        try:
            s.connect((STREAM_HOST, STREAM_PORT))
            s.close()
            print(f"[DAEMON] pc_main.exe successfully started (PID {proc.pid}) and listening on port {STREAM_PORT}!")
            break
        except Exception:
            pass
    return proc

def main():
    global g_running
    print("=" * 60)
    print("  Antigravity CarPlay PC Viewer (Phase B.1)")
    print("  Resolution: 1024x600 | 30 FPS | Bidirectional Touch")
    print("=" * 60)

    daemon_proc = ensure_daemon()

    # Start control thread
    ctrl_thread = threading.Thread(target=connect_control, daemon=True)
    ctrl_thread.start()

    cv2.namedWindow(WIN_NAME, cv2.WINDOW_AUTOSIZE)
    cv2.setMouseCallback(WIN_NAME, on_mouse)

    # Initial placeholder screen
    placeholder = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    placeholder[:] = (30, 30, 30) # Dark gray
    cv2.putText(placeholder, "Connecting to CarPlay Stream (127.0.0.1:18888)...",
                (WIDTH // 2 - 380, HEIGHT // 2 - 20),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (200, 200, 200), 2)
    cv2.putText(placeholder, "Make sure iPhone is connected to Carlinkit box",
                (WIDTH // 2 - 320, HEIGHT // 2 + 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (150, 150, 150), 1)
    cv2.imshow(WIN_NAME, placeholder)
    cv2.waitKey(1)

    url = f"tcp://{STREAM_HOST}:{STREAM_PORT}"
    print(f"[VIDEO] Connecting to video stream {url}...")

    cap = None
    retry_count = 0
    while g_running:
        if cap is None or not cap.isOpened():
            cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
            if not cap.isOpened():
                retry_count += 1
                if retry_count % 15 == 0:
                    print(f"[STATUS] Waiting for iPhone video stream... (Make sure iPhone is unlocked & cold-booted with box)")
                cv2.imshow(WIN_NAME, placeholder)
                key = cv2.waitKey(200) & 0xFF
                if key == ord('q') or key == 27:
                    break
                continue
            print(f"[VIDEO] Stream opened! Rendering live CarPlay...")

        ret, frame = cap.read()
        if not ret:
            # Stream temporarily interrupted, wait for next keyframe
            time.sleep(0.01)
            key = cv2.waitKey(10) & 0xFF
            if key == ord('q') or key == 27:
                break
            continue

        # Resize if dimensions differ slightly
        if frame.shape[0] != HEIGHT or frame.shape[1] != WIDTH:
            frame = cv2.resize(frame, (WIDTH, HEIGHT))

        cv2.imshow(WIN_NAME, frame)
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q') or key == 27:
            break

    print("[SHUTDOWN] Exiting CarPlay Viewer...")
    g_running = False
    if cap:
        cap.release()
    cv2.destroyAllWindows()
    if daemon_proc:
        try:
            daemon_proc.terminate()
        except Exception:
            pass

if __name__ == "__main__":
    main()
