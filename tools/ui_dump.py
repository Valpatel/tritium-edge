#!/usr/bin/env python3
"""
Quick UI widget dump for Tritium-OS.
Prints all widgets with their values in a readable format.

Usage:
    python3 tools/ui_dump.py [device_ip]
    python3 tools/ui_dump.py 192.168.86.50 --json     # Raw JSON
    python3 tools/ui_dump.py 192.168.86.50 --click ID  # Click a widget
    python3 tools/ui_dump.py 192.168.86.50 --app Settings  # Launch app first
"""
import argparse
import json
import sys

try:
    import requests
except ImportError:
    print("pip install requests")
    sys.exit(1)


def auto_detect(port=80):
    for h in ["tritium.local", "192.168.86.50", "192.168.4.1",
              "192.168.1.100", "10.42.0.2"]:
        try:
            r = requests.get(f"http://{h}:{port}/api/remote/info", timeout=1.5)
            if r.status_code == 200:
                return h
        except Exception:
            pass
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("host", nargs="?")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--json", action="store_true", help="Raw JSON output")
    p.add_argument("--app", help="Launch app before dumping")
    p.add_argument("--click", help="Click widget by ID")
    p.add_argument("--set", nargs=2, metavar=("ID", "VALUE"), help="Set widget value")
    p.add_argument("--home", action="store_true", help="Go to launcher")
    args = p.parse_args()

    host = args.host or auto_detect(args.port)
    if not host:
        print("No device found", file=sys.stderr)
        sys.exit(1)

    base = f"http://{host}:{args.port}"
    session = requests.Session()

    if args.home:
        r = session.post(f"{base}/api/shell/home", json={})
        print(r.json())
        return

    if args.app:
        r = session.post(f"{base}/api/shell/launch", json={"name": args.app})
        import time; time.sleep(0.4)

    if args.click:
        r = session.post(f"{base}/api/ui/click", json={"id": args.click})
        print(json.dumps(r.json(), indent=2))
        import time; time.sleep(0.3)

    if args.set:
        wid, val = args.set
        # Try integer first, then string, then bool
        data = {"id": wid}
        if val.lower() in ("true", "false"):
            data["checked"] = val.lower() == "true"
        else:
            try:
                data["value"] = int(val)
            except ValueError:
                data["text"] = val
        r = session.post(f"{base}/api/ui/set", json=data)
        print(json.dumps(r.json(), indent=2))
        import time; time.sleep(0.2)

    # Dump widget tree
    r = session.get(f"{base}/api/ui/tree?flat=1")
    widgets = r.json()

    if args.json:
        print(json.dumps(widgets, indent=2))
        return

    if not isinstance(widgets, list):
        print(f"Error: {widgets}")
        return

    # Pretty print
    type_colors = {
        "btn": "\033[36m",       # cyan
        "label": "\033[37m",     # white
        "slider": "\033[33m",    # yellow
        "switch": "\033[32m",    # green
        "bar": "\033[35m",       # magenta
        "dropdown": "\033[34m",  # blue
        "textarea": "\033[33m",  # yellow
        "checkbox": "\033[32m",  # green
    }
    reset = "\033[0m"

    print(f"{'ID':>12}  {'Type':<18} {'Pos':>10}  {'Size':>8}  Value/Text")
    print("-" * 80)

    for w in widgets:
        wtype = w.get("type", "?")
        color = type_colors.get(wtype, "")
        wid = w.get("id", "")[-10:]
        pos = f"{w.get('x',0)},{w.get('y',0)}"
        size = f"{w.get('w',0)}x{w.get('h',0)}"
        depth = w.get("depth", 0)
        indent = "  " * min(depth, 6)

        # Value column
        val = ""
        if "text" in w:
            val = w["text"][:40]
        if "value" in w:
            val = f"value={w['value']}"
            if "min" in w:
                val += f" [{w['min']}-{w['max']}]"
        if "checked" in w:
            val = f"{'ON' if w['checked'] else 'OFF'}"
        if "selected_text" in w:
            val = f"selected: {w['selected_text']}"
        if w.get("clickable"):
            val = f"[click] {val}"

        print(f"{wid:>12}  {color}{indent}{wtype:<{18-len(indent)}}{reset} {pos:>10}  {size:>8}  {val}")

    print(f"\n{len(widgets)} widgets total, "
          f"{sum(1 for w in widgets if w.get('clickable'))} interactive")


if __name__ == "__main__":
    main()
