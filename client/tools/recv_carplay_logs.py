#!/usr/bin/env python3
"""
recv_carplay_logs.py — Real-time UDP log listener for Tang CarPlay on WinCE.

Usage:
  python client/tools/recv_carplay_logs.py [port]

Description:
  Listens for UDP broadcast log datagrams sent by carlinkit_ce.exe over the
  vehicle router network (port 9999 by default) and prints them in real time.
"""

import socket
import sys
import datetime

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", port))
    except Exception as e:
        print(f"[-] Failed to bind UDP port {port}: {e}")
        return 1

    print(f"=======================================================")
    print(f" [*] Tang CarPlay Real-Time UDP Log Listener Active")
    print(f" [*] Listening on 0.0.0.0:{port} (Vehicle Router Network)")
    print(f" [*] Waiting for carlinkit_ce.exe to stream logs...")
    print(f"=======================================================\n")

    while True:
        try:
            data, addr = sock.recvfrom(4096)
            text = data.decode("utf-8", errors="replace").rstrip("\r\n")
            print(f"[{addr[0]}] {text}")
            sys.stdout.flush()
        except KeyboardInterrupt:
            print("\n[*] Listener stopped by user.")
            break
        except Exception as e:
            print(f"[-] Error: {e}")

    sock.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
