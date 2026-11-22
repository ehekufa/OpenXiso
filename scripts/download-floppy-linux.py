#!/usr/bin/env python3
"""Download the original experimental Floppy Linux image, without committing it."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import urllib.request

COMMIT = "10c84bfc1f48b6f9ba9d4355327da6d5d4b32522"
URL = ("https://api.github.com/repos/iczelia/floppy-linux/contents/"
       f"floppy-linux.img?ref={COMMIT}")
SHA256 = "f7e0ede3d01e2dd849e9b3a1d0b68ced223cd4174d70c7499b902ffac4b4dd01"
SIZE = 1474560


def main():
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("test-images/floppy-linux.img")
    if len(sys.argv) > 2:
        raise SystemExit("Usage: download-floppy-linux.py [output.img]")
    target.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(URL, headers={"Accept": "application/vnd.github.raw+json",
                                                  "User-Agent": "OpenXiso-linux-test"})
    with urllib.request.urlopen(request, timeout=60) as response:
        image = response.read(SIZE + 1)
    if len(image) != SIZE or hashlib.sha256(image).hexdigest() != SHA256:
        raise SystemExit("Downloaded image has unexpected size or SHA-256; not saved")
    fd, temp = tempfile.mkstemp(dir=target.parent, prefix=".floppy-linux-")
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(image)
        os.replace(temp, target)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    print(f"Saved {target} ({SIZE} bytes, SHA-256 {SHA256})")
    print("Experimental: OpenXiso does not yet boot this image to a Linux shell.")


if __name__ == "__main__":
    main()
