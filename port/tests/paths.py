"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))

# The launcher keeps its settings in ~/.config/infamous-launcher (APPDATA on Windows), outside
# ISS_USER_DIR: without this, a test that saves settings overwrote the player's real file (game
# folder, language). Every test module imports this one, so they all get a throwaway folder; a
# test that needs its own still patches XDG_CONFIG_HOME/APPDATA itself.
import atexit as _atexit
import os as _os
import shutil as _shutil
import tempfile as _tempfile

_CONFIG = _tempfile.mkdtemp(prefix='iss-test-config-')
_os.environ['XDG_CONFIG_HOME'] = _CONFIG
_os.environ['APPDATA'] = _CONFIG
_atexit.register(_shutil.rmtree, _CONFIG, True)
