"""Compress the self-contained local setup page deterministically for Flash."""
import gzip
import sys
from pathlib import Path

source, destination = map(Path, sys.argv[1:])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
