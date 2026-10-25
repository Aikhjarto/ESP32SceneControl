# PlatformIO pre-build step: gzip the web UI so the firmware can embed it and serve it
# compressed. Output is deterministic (mtime=0) and only rewritten
# when the content changes, so unchanged UI files do not trigger a relink.
Import("env")  # noqa: F821 (provided by PlatformIO)

import gzip
import os

WEB_FILES = ("index.html", "app.js", "style.css")

web_dir = os.path.join(env.subst("$PROJECT_DIR"), "web")  # noqa: F821
dist_dir = os.path.join(web_dir, "dist")
os.makedirs(dist_dir, exist_ok=True)

for name in WEB_FILES:
    with open(os.path.join(web_dir, name), "rb") as f:
        data = f.read()
    packed = gzip.compress(data, compresslevel=9, mtime=0)
    out = os.path.join(dist_dir, name + ".gz")
    if os.path.exists(out):
        with open(out, "rb") as f:
            if f.read() == packed:
                continue
    with open(out, "wb") as f:
        f.write(packed)
    print("embed_web: %s %d -> %d bytes" % (name, len(data), len(packed)))
