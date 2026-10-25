"""Save editor (scripts/save_editor.py) on sample saves from this build (tests/data/savedata/CUSA02320:
U1 2% El Dorado, U2 train chapter 1, U3 London pub, the three profiles and the shared profile)."""
from paths import ROOT
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

import save_editor as se

DATA = ROOT / 'tests' / 'data' / 'savedata' / 'CUSA02320'
SLOTS = ['SAVEDATA010', 'SAVEDATA01P', 'SAVEDATA020', 'SAVEDATA02P', 'SAVEDATA030', 'SAVEDATA03P', 'SAVEDATA0CC']


def plain(path):
    return se.decrypt(Path(path).read_bytes())


class CryptoTests(unittest.TestCase):
    def test_round_trip_is_byte_identical(self):
        for slot in SLOTS:
            raw = (DATA / slot / 'USR-DATA').read_bytes()
            d = se.decrypt(raw)
            self.assertTrue(se.crc_ok(d), slot)
            self.assertTrue(se.hmac_ok(d), slot)
            self.assertEqual(bytes(se.encrypt(d)), raw, slot)
            fixed = bytearray(d)
            se.fix_checksums(fixed)
            self.assertEqual(fixed, d, slot)

    def test_pure_python_blowfish_matches(self):
        raw = (DATA / 'SAVEDATA01P' / 'USR-DATA').read_bytes()
        n = se._dsize(raw)
        pure = se._crypt(raw[12:12 + n], False, pure=True)
        self.assertEqual(se._crypt(pure, True, pure=True), raw[12:12 + n])
        try:
            import Crypto  # noqa: F401
        except ImportError:
            self.skipTest('pycryptodome not installed')
        self.assertEqual(pure, se._crypt(raw[12:12 + n], False))

    def test_string_ids(self):
        self.assertEqual(se.sid('game-difficulty'), 0x3E7FDCC8)
        self.assertEqual(se.sid('*unlockables*'), 0xFA841429)
        self.assertEqual(se.sid('kill-total'), 0xD76261F5)

    def test_rejects_damaged_file(self):
        raw = bytearray((DATA / 'SAVEDATA02P' / 'USR-DATA').read_bytes())
        raw[0x700] ^= 0xFF
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 'SAVEDATA02P' / 'USR-DATA'
            f.parent.mkdir()
            f.write_bytes(raw)
            with self.assertRaises(se.SaveError):
                se.Save(f)


class ParseTests(unittest.TestCase):
    def test_sections(self):
        expect = {'SAVEDATA010': {300, 308, 0x139}, 'SAVEDATA01P': {316, 309, 312}, 'SAVEDATA020': {300, 308},
                  'SAVEDATA02P': {316, 315}, 'SAVEDATA030': {300, 308}, 'SAVEDATA03P': {316}, 'SAVEDATA0CC': set()}
        for slot, types in expect.items():
            s = se.Save(DATA / slot / 'USR-DATA')
            self.assertTrue(types <= set(s.sections), (slot, sorted(s.sections)))
            self.assertIn(se.sid('create-type'), s.pairs)

    def test_describe(self):
        r = se.describe(DATA)
        self.assertEqual(r['errors'], [])
        u1, u1p = r['saves']['SAVEDATA010'], r['saves']['SAVEDATA01P']
        self.assertEqual((u1['game'], u1['kind'], u1['chapter'], u1['campaign_percent']), ('u1', 'game', 'The Search for El Dorado', 2))
        self.assertEqual((u1['playtime_minutes'], u1['difficulty'], u1['tasks_total'], u1['tasks_played']), (13, 'Easy', 350, 8))
        self.assertEqual((u1['treasures_found'], u1['treasures_total'], u1['all_chapters_selectable']), (0, 61, False))
        self.assertEqual((u1p['rewards_total'], u1p['rewards_unlocked'], u1p['medals_total'], u1p['brutal_unlocked']), (48, 0, 62, False))
        u2, u3 = r['saves']['SAVEDATA020'], r['saves']['SAVEDATA030']
        self.assertEqual((u2['chapter'], u2['difficulty'], u2['tasks_total']), ('A Rock and a Hard Place', 'Normal', 492))
        self.assertEqual((u3['chapter'], u3['playtime_minutes'], u3['tasks_total']), ('Another Round', 6, 559))
        self.assertEqual(r['saves']['SAVEDATA03P']['completed_difficulties'], [])


class EditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.title = Path(self.tmp.name) / 'savedata' / '1' / 'CUSA02320'
        shutil.copytree(DATA, self.title)
        self.before = {slot: (self.title / slot / 'USR-DATA').read_bytes() for slot in SLOTS}

    def tearDown(self):
        self.tmp.cleanup()

    def check_only(self, slot, changes):
        """Plaintext differs only in the edited u32s and the two checksums; the file size is the same;
        ciphertext blocks without a change are untouched; the backup holds the original."""
        old_raw, new_raw = self.before[slot], (self.title / slot / 'USR-DATA').read_bytes()
        self.assertEqual(len(old_raw), len(new_raw))
        old, new = se.decrypt(old_raw), se.decrypt(new_raw)
        dsize = se._dsize(new)
        allowed = set(range(se.CRC_AT, se.CRC_AT + 4)) | set(range(dsize - 8, dsize + 12))
        mine = [c for c in changes if Path(c['file']).parent.name == slot]
        for c in mine:
            allowed |= set(range(c['offset'], c['offset'] + 4))
            self.assertEqual(struct.unpack_from('<I', new, c['offset'])[0], c['new'])
            self.assertEqual(struct.unpack_from('<I', old, c['offset'])[0], c['old'])
        diff = {i for i in range(len(old)) if old[i] != new[i]}
        self.assertTrue(diff <= allowed, (slot, sorted(diff - allowed)[:8]))
        blocks = {(i - 12) // 8 for i in allowed if i >= 12}
        for b in range(dsize // 8):
            if b not in blocks:
                self.assertEqual(old_raw[12 + 8 * b:20 + 8 * b], new_raw[12 + 8 * b:20 + 8 * b])
        self.assertTrue(se.crc_ok(new) and se.hmac_ok(new))
        self.assertEqual(Path(mine[0]['backup']).read_bytes(), old_raw)
        self.assertTrue(Path(mine[0]['backup']).parent.parent.parent.name.endswith('.backups'))

    def test_dry_run_writes_nothing(self):
        changes = se.unlock_chapters(self.title, dry_run=True) + se.unlock_difficulties(self.title, dry_run=True) \
            + se.unlock_rewards(self.title, dry_run=True)
        self.assertTrue(changes)
        for slot in SLOTS:
            self.assertEqual((self.title / slot / 'USR-DATA').read_bytes(), self.before[slot])
        self.assertFalse((self.title.parent / 'CUSA02320.backups').exists())

    def test_unlock_chapters(self):
        changes = se.unlock_chapters(self.title)
        for slot, game, value in (('SAVEDATA010', 'u1', 1), ('SAVEDATA020', 'u2', 2), ('SAVEDATA030', 'u3', 2)):
            self.check_only(slot, changes)
            s = se.Save(self.title / slot / 'USR-DATA')
            old = dict((e[0], e[7]) for _, e in se.Save(DATA / slot / 'USR-DATA').entries(300, 32))
            for _, e in s.entries(300, 32):
                self.assertEqual(e[7], old[e[0]] if old[e[0]] != 0xFFFFFFFF else value)  # played tasks keep theirs
            self.assertTrue(se.describe(self.title / slot)['saves'][slot]['all_chapters_selectable'])
        for slot in ('SAVEDATA01P', 'SAVEDATA02P', 'SAVEDATA03P', 'SAVEDATA0CC'):
            self.assertEqual((self.title / slot / 'USR-DATA').read_bytes(), self.before[slot])
        self.assertEqual(se.unlock_chapters(self.title), [])  # nothing left to do

    def test_unlock_chapters_one_game_and_difficulty(self):
        changes = se.unlock_chapters(self.title, difficulty=3, game='u2')
        self.assertEqual({Path(c['file']).parent.name for c in changes}, {'SAVEDATA020'})
        self.assertEqual({c['new'] for c in changes}, {3})
        self.check_only('SAVEDATA020', changes)

    def test_unlock_difficulties(self):
        changes = se.unlock_difficulties(self.title)
        r = se.describe(self.title)['saves']
        for slot in ('SAVEDATA01P', 'SAVEDATA02P', 'SAVEDATA03P'):
            self.check_only(slot, changes)
            self.assertTrue(r[slot]['brutal_unlocked'] and r[slot]['crushing_unlocked'], slot)
            self.assertEqual(r[slot]['completed_difficulties'], ['Hard', 'Crushing'])
        self.assertEqual(r['SAVEDATA01P']['rewards_unlocked'], 1)  # the Crushing reward only
        for slot in ('SAVEDATA010', 'SAVEDATA020', 'SAVEDATA030', 'SAVEDATA0CC'):
            self.assertEqual((self.title / slot / 'USR-DATA').read_bytes(), self.before[slot])

    def test_unlock_rewards(self):
        changes = se.unlock_rewards(self.title)
        self.check_only('SAVEDATA01P', changes)
        s = se.Save(self.title / 'SAVEDATA01P' / 'USR-DATA')
        for _, e in s.entries(312, 12):
            self.assertEqual(e[2] & 0x1F, 0x0F)  # unlocked, none turned on
        self.assertEqual(se.describe(self.title)['saves']['SAVEDATA01P']['rewards_locked'], [])
        with self.assertRaises(se.SaveError):
            se.unlock_rewards(self.title, game='u2')

    def test_cli(self):
        self.assertEqual(se.main(['unlock-all', str(self.title.parent.parent.parent), '--dry-run', '--json']), 0)
        self.assertEqual(se.main(['unlock-all', str(self.title), '--game', 'u3']), 0)
        self.assertNotEqual((self.title / 'SAVEDATA030' / 'USR-DATA').read_bytes(), self.before['SAVEDATA030'])
        self.assertEqual((self.title / 'SAVEDATA010' / 'USR-DATA').read_bytes(), self.before['SAVEDATA010'])
        self.assertEqual(se.main(['describe', str(self.title / 'SAVEDATA030')]), 0)
        self.assertEqual(se.main(['describe', str(Path(self.tmp.name) / 'nothing')]), 0)


if __name__ == '__main__':
    unittest.main()
