"""Compatibility helper is maintained in the pinned shared gateway repository."""
from pathlib import Path
import runpy

runpy.run_path(str(Path(__file__).resolve().parents[2] / "integrations/valet-gateway/tools/enable_valet.py"), run_name="__main__")
