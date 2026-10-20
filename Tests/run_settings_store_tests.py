"""Compatibility entry: the current settings backend is external W25Q."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name("run_external_storage_tests.py")), run_name="__main__")
