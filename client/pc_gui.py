#!/usr/bin/env python3
"""
pc_gui.py — Antigravity CarPlay PC GUI (Phase B.1 Live Video + Touch Screen + Audio)

Features:
- Thread-decoupled architecture: video receiver & touch sender run in background threads.
- GUI thread runs uninterrupted at 60 FPS, ensuring window NEVER enters "Not Responding".
- Connects to pc_main.exe H.264 video stream over TCP (127.0.0.1:18888).
- Connects to pc_main.exe control port (127.0.0.1:18889) with framed touch events.
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
import queue
import cv2
import numpy as np

STREAM_HOST = "127.0.0.1"
STREAM_PORT = 18888
CTRL_HOST = "127.0.0.1"
CTRL_PORT = 18889
WIN_NAME = "Antigravity CarPlay (1024x600) | Audio & Touch Enabled"
WIDTH = 1024
HEIGHT = 600

g_running = True
g_latest_frame = None
frame_lock = threading.Lock()
g_stream_active = False

touch_queue = queue.Queue(maxsize=200)
last_touch = [0, 0]

def on_mouse(event, x, y, flags, param):
    """Translate window mouse events to normalized 0..10000 CarPlay touch events."""
    norm_x = int(max(0, min(10000, (x / float(WIDTH)) * 10000)))
    norm_y = int(max(0, min(10000, (y / float(HEIGHT)) * 10000)))

    if event == cv2.EVENT_LBUTTONDOWN:
        last_touch[0], last_touch[1] = norm_x, norm_y
        print(f"[TOUCH-DOWN] x={norm_x} y={norm_y} (px: {x},{y})")
        try:
            touch_queue.put_nowait((14, norm_x, norm_y))
        except queue.Full:
            pass
    elif event == cv2.EVENT_MOUSEMOVE and (flags & cv2.EVENT_FLAG_LBUTTON):
        # Throttle mouse move jitter (at least 20 units / 0.2% change)
        if abs(norm_x - last_touch[0]) >= 20 or abs(norm_y - last_touch[1]) >= 20:
            last_touch[0], last_touch[1] = norm_x, norm_y
            try:
                touch_queue.put_nowait((15, norm_x, norm_y))
            except queue.Full:
                pass
    elif event == cv2.EVENT_LBUTTONUP:
        last_touch[0], last_touch[1] = norm_x, norm_y
        print(f"[TOUCH-UP  ] x={norm_x} y={norm_y} (px: {x},{y})")
        try:
            touch_queue.put_nowait((16, norm_x, norm_y))
        except queue.Full:
            pass

def touch_sender():
    """Background worker sending queued touch events to pc_main control port."""
    global g_running
    sock = None
    while g_running:
        if sock is None:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((CTRL_HOST, CTRL_PORT))
                s.settimeout(0.5)
                sock = s
                print(f"[CTRL] Connected to control channel at {CTRL_HOST}:{CTRL_PORT}")
            except Exception:
                time.sleep(0.5)
                continue

        try:
            action, tx, ty = touch_queue.get(timeout=0.1)
            payload = struct.pack("<III", action, tx, ty)
            sock.sendall(payload)
        except queue.Empty:
            pass
        except Exception as e:
            print(f"[CTRL] Connection dropped: {e}")
            try:
                sock.close()
            except Exception:
                pass
            sock = None

    if sock:
        try:
            sock.close()
        except Exception:
            pass

def video_receiver():
    """Background worker receiving H.264 stream via OpenCV FFmpeg."""
    global g_latest_frame, g_stream_active, g_running
    url = f"tcp://{STREAM_HOST}:{STREAM_PORT}"
    print(f"[VIDEO] Connecting to video stream {url}...")

    retry_count = 0
    while g_running:
        cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
        if not cap.isOpened():
            g_stream_active = False
            retry_count += 1
            if retry_count % 10 == 0:
                print(f"[STATUS] Waiting for CarPlay stream at {url}...")
            time.sleep(0.5)
            continue

        g_stream_active = True
        print(f"[VIDEO] Stream opened! Rendering live CarPlay...")

        while g_running:
            ret, frame = cap.read()
            if not ret:
                # Stream paused or waiting for keyframe
                time.sleep(0.01)
                continue

            if frame.shape[0] != HEIGHT or frame.shape[1] != WIDTH:
                frame = cv2.resize(frame, (WIDTH, HEIGHT))

            with frame_lock:
                g_latest_frame = frame

        cap.release()
        time.sleep(0.2)

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
    print("  Resolution: 1024x600 | 30 FPS | Audio & Bidirectional Touch")
    print("=" * 60)

    daemon_proc = ensure_daemon()

    # Start background threads
    ctrl_thread = threading.Thread(target=touch_sender, daemon=True)
    ctrl_thread.start()

    vid_thread = threading.Thread(target=video_receiver, daemon=True)
    vid_thread.start()

    cv2.namedWindow(WIN_NAME, cv2.WINDOW_AUTOSIZE)
    cv2.setMouseCallback(WIN_NAME, on_mouse)

    # Placeholder screen
    placeholder = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    placeholder[:] = (30, 30, 30) # Dark gray
    cv2.putText(placeholder, "Connecting to CarPlay Stream (127.0.0.1:18888)...",
                (WIDTH // 2 - 380, HEIGHT // 2 - 20),
                cv2.FONT_HERSHEY_SIMPLEX, 0.8, (200, 200, 200), 2)
    cv2.putText(placeholder, "Make sure iPhone is connected to Carlinkit box",
                (WIDTH // 2 - 320, HEIGHT // 2 + 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (150, 150, 150), 1)

    # Main thread: continuous 60 FPS render loop
    while g_running:
        with frame_lock:
            frame_to_render = g_latest_frame

        if frame_to_render is not None:
            cv2.imshow(WIN_NAME, frame_to_render)
        else:
            cv2.imshow(WIN_NAME, placeholder)

        # Message pump: 16ms waitKey guarantees smooth window responsiveness
        key = cv2.waitKey(16) & 0xFF
        if key == ord('q') or key == 27:
            break

    print("[SHUTDOWN] Exiting CarPlay Viewer...")
    g_running = False
    cv2.destroyAllWindows()
    if daemon_proc:
        try:
            daemon_proc.terminate()
        except Exception:
            pass

if __name__ == "__main__":
    main()
