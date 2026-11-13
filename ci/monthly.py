#!/usr/bin/env python3
"""Import the newest kernel.org release of each vanilla line in vmbind.toml
and report whether the line's stacks still apply. Used by monthly.yml."""
import json, subprocess, sys, tomllib, urllib.request

m = tomllib.load(open("vmbind.toml", "rb"))
rel = json.load(urllib.request.urlopen("https://www.kernel.org/releases.json"))
latest = {".".join(r["version"].split(".")[:2]): r["version"]
          for r in rel["releases"] if r["moniker"] in ("stable", "longterm")}
have = {t["id"] for t in m["targets"]}
print("| line | new release | stacks |\n|---|---|---|")
for line in sorted({t["line"] for t in m["targets"] if t.get("flavour") == "vanilla"}):
    v = latest.get(line)
    if not v or v in have:
        continue
    subprocess.run([sys.executable, "vmbind.py", "import", v], check=True)
    r = subprocess.run([sys.executable, "vmbind.py", "check", v], capture_output=True, text=True)
    print(f"| {line} | {v} | {'apply cleanly' if r.returncode == 0 else 'CONFLICT: run vmbind.py start ' + v} |")
