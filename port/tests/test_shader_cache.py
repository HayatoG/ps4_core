"""launcher/shader_cache.py: only this game's regenerable GPU cache is found and deleted."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import shader_cache  # noqa: E402


def write(path, size):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'x' * size)


def param_sfo(title_id):
    """A minimal param.sfo with one TITLE_ID string entry."""
    key, value = b'TITLE_ID\0', title_id.encode() + b'\0' * (16 - len(title_id))
    keys = 20 + 16
    values = keys + len(key) + (-len(key) % 4)
    header = struct.pack('<4sIIII', b'\0PSF', 0x101, keys, values, 1)
    entry = struct.pack('<HHIII', 0, 0x204, len(title_id) + 1, 16, 0)
    return header + entry + key + b'\0' * (values - keys - len(key)) + value


class ShaderCacheTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.user = self.root / 'port' / 'user'
        self.env = {'HOME': str(self.root / 'home'), 'XDG_DATA_HOME': str(self.root / 'xdg')}
        shared = self.root / 'xdg' / 'shadPS4' / 'cache'
        # What must survive: saves, settings, logs, shader patches, other games' caches and, in
        # the shadPS4 folder (an installed emulator may own it), anything but our keyed folders.
        self.keep = [self.user / 'savedata' / 'SAVE0' / 'data.bin', self.user / 'tlg.ini',
                     self.user / 'log' / 'run.log', self.user / 'shader' / 'patch' / 'a.xml',
                     self.user / 'cache' / 'CUSA00223-be647011b7db' / 'a.spv',
                     shared / 'CUSA00223-be647011b7db' / 'a.spv',
                     shared / 'CUSA036270-e3bd0f4d2138' / 'a.spv',
                     shared / 'UNKNOWN-72a09eaa1980' / 'a.spv',
                     shared / 'CUSA03627' / 'pipeline-binaries' / 'b.bin',
                     shared / 'CUSA03627.zip',
                     shared / 'CUSA03627-custom' / 'a.spv']
        for path in self.keep:
            write(path, 10)
        # This game's cache: the port's folder (every layout) and the old locations it owns.
        self.cache = [self.user / 'cache' / 'CUSA03627-e3bd0f4d2138' / 'a.spv',
                      self.user / 'cache' / 'CUSA03627-e3bd0f4d2138' / 'pipeline-binaries' / 'b.bin',
                      self.user / 'cache' / 'CUSA03627' / 'c.spv',
                      self.user / 'user' / 'cache' / 'CUSA03627.zip',
                      shared / 'CUSA03627-0123456789ab' / 'a.spv']
        for path in self.cache:
            write(path, 1000)

    def tearDown(self):
        self.folder.cleanup()

    def test_size_and_clear_touch_only_this_game(self):
        if os.name == 'nt':
            self.skipTest('the XDG location is Linux only')
        self.assertEqual(shader_cache.size(['CUSA03627'], self.user, self.env), 5000)
        freed, failures = shader_cache.clear(['CUSA03627'], self.user, self.env)
        self.assertEqual((freed, failures), (5000, []))
        for path in self.cache:
            self.assertFalse(path.exists(), path)
        for path in self.keep:
            self.assertTrue(path.exists(), path)
        self.assertEqual(shader_cache.clear(['CUSA03627'], self.user, self.env), (0, []))

    def test_files_that_cannot_go_are_reported(self):
        if os.name == 'nt' or os.geteuid() == 0:
            self.skipTest('needs POSIX permissions without root')
        locked = self.user / 'cache' / 'CUSA03627'
        locked.chmod(0o500)  # like a file still in use on Windows: c.spv cannot be removed
        try:
            freed, failures = shader_cache.clear(['CUSA03627'], self.user, self.env)
        finally:
            locked.chmod(0o700)
        self.assertEqual(freed, 4000 if os.name != 'nt' else 3000)
        self.assertTrue(failures)
        self.assertTrue((locked / 'c.spv').exists())

    def test_other_gpu_user_dir(self):
        other = self.root / 'gpu'
        write(other / 'cache' / 'CUSA03627-abc' / 'd.spv', 500)
        found = shader_cache.entries(['CUSA03627'], other, self.env)
        self.assertIn(other / 'cache' / 'CUSA03627-abc', found)
        self.assertNotIn(self.user / 'cache' / 'CUSA03627', found)

    def test_bad_serials_match_nothing(self):
        for serials in ([], [''], ['CUSA'], ['*'], [None]):
            with self.subTest(serials=serials):
                self.assertEqual(shader_cache.entries(serials, self.user, self.env), [])

    def test_title_id(self):
        game = self.root / 'game'
        (game / 'sce_sys').mkdir(parents=True)
        self.assertIsNone(shader_cache.title_id(game))
        (game / 'sce_sys' / 'param.sfo').write_bytes(param_sfo('CUSA03627'))
        self.assertEqual(shader_cache.title_id(game), 'CUSA03627')
        (game / 'sce_sys' / 'param.sfo').write_bytes(b'garbage')
        self.assertIsNone(shader_cache.title_id(game))

    def test_human_size(self):
        self.assertEqual(shader_cache.human_size(512), '512 B')
        self.assertEqual(shader_cache.human_size(3 * 1024 * 1024), '3.0 MB')
        self.assertEqual(shader_cache.human_size(5 * 1024 ** 3), '5.00 GB')


if __name__ == '__main__':
    unittest.main()
