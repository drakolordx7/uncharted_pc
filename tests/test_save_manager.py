"""Save backups (launcher/uncharted_saves.py): backup, ZIP checks, atomic restore and rollback."""
from paths import ROOT
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_saves as saves  # noqa: E402

TITLE = 'CUSA02320'


def param(subtitle, title='Uncharted', detail=''):
    data = bytearray(1328)
    for offset, size, text in ((0, 128, title), (128, 128, subtitle), (256, 1024, detail)):
        raw = text.encode()
        data[offset:offset + len(raw)] = raw
    struct.pack_into('<Iiq', data, 1280, 0, 0, 1792092050)
    return bytes(data)


def make_save(user, name, content, subtitle='Campaign 2% Complete - The Search for El Dorado'):
    base = Path(user) / 'savedata' / '1' / TITLE
    (base / name).mkdir(parents=True, exist_ok=True)
    (base / name / 'USR-DATA').write_bytes(content)
    (base / f'{name}.sce_sys').mkdir(exist_ok=True)
    (base / f'{name}.sce_sys' / 'param.bin').write_bytes(param(subtitle))


def usr(user, name):
    return (Path(user) / 'savedata' / '1' / TITLE / name / 'USR-DATA').read_bytes()


def craft_zip(path, members, manifest):
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr(saves.MANIFEST, json.dumps(manifest))
        for name, data in members.items():
            archive.writestr(name, data)


class SaveManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name) / 'user'
        make_save(self.user, 'SAVEDATA010', b'u1-game-v1')
        make_save(self.user, 'SAVEDATA01P', b'u1-profile-v1', 'Profile')
        make_save(self.user, 'SAVEDATA030', b'u3-game-v1', 'Campaign 16% Complete - London Underground')
        make_save(self.user, 'SAVEDATA0CC', b'shared-v1', 'Shared Profile')
        (self.user / 'trophy').mkdir()
        (self.user / 'trophy' / 'NPWR09999_00.log').write_text('1 100\n')

    def tearDown(self):
        self.temp.cleanup()

    def test_list_shows_game_slot_and_progress(self):
        found = {s['dir']: s for s in saves.list_saves(self.user)}
        self.assertEqual(set(found), {'SAVEDATA010', 'SAVEDATA01P', 'SAVEDATA030', 'SAVEDATA0CC'})
        self.assertEqual(found['SAVEDATA010']['game'], 'u1')
        self.assertEqual(found['SAVEDATA010']['slot'], '0')
        self.assertEqual(found['SAVEDATA0CC']['game'], 'shared')
        self.assertIn('El Dorado', found['SAVEDATA010']['subtitle'])
        described = saves.list_saves(self.user, describe=lambda p: {'path': p})
        self.assertTrue(all(s['details'] for s in described))
        broken = saves.list_saves(self.user, describe=lambda p: 1 / 0)
        self.assertTrue(all(s['details'] is None for s in broken))

    def test_game_backup_holds_only_that_game_with_hashes(self):
        path = saves.create_backup(self.user, 'u1', game={'id': TITLE, 'version': '01.02'})
        self.assertRegex(path.name, r'^u1-\d{8}-\d{6}\.zip$')
        self.assertEqual(path.parent, self.user / 'backups')
        manifest = saves.validate_backup(path)
        self.assertEqual(manifest['game'], 'u1')
        self.assertEqual(manifest['game_version'], '01.02')
        self.assertEqual(manifest['title_ids'], [TITLE])
        names = {f['path'] for f in manifest['files']}
        self.assertTrue(all('/SAVEDATA01' in n for n in names), names)
        self.assertTrue(all(len(f['sha256']) == 64 for f in manifest['files']))

    def test_all_backup_includes_trophies(self):
        manifest = saves.validate_backup(saves.create_backup(self.user, 'all'))
        self.assertIn('trophy/NPWR09999_00.log', {f['path'] for f in manifest['files']})

    def test_rejects_path_traversal_and_foreign_files(self):
        bad_names = ['../evil.txt', 'savedata/1/CUSA02320/SAVEDATA010/../../../../x', '/abs/USR-DATA',
                     'C:/Windows/x', 'savedata\\1\\CUSA02320\\SAVEDATA010\\USR-DATA', 'run.py',
                     'savedata/1/CUSA02320/SAVEDATA010']
        for number, name in enumerate(bad_names):
            with self.subTest(name=name):
                path = Path(self.temp.name) / f'bad{number}.zip'
                craft_zip(path, {name: b'x'}, {'format': saves.FORMAT, 'game': 'all',
                                               'files': [{'path': name, 'size': 1, 'sha256': ''}]})
                with self.assertRaises(saves.SaveError):
                    saves.validate_backup(path)

    def test_rejects_other_game_wrong_title_damage_and_non_backups(self):
        good = 'savedata/1/CUSA02320/SAVEDATA030/USR-DATA'
        path = Path(self.temp.name) / 'mixed.zip'
        craft_zip(path, {good: b'x'}, {'format': saves.FORMAT, 'game': 'u1', 'files': [
            {'path': good, 'size': 1, 'sha256': '2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881'}]})
        with self.assertRaisesRegex(saves.SaveError, "another game"):
            saves.validate_backup(path)
        backup = saves.create_backup(self.user, 'u1')
        with self.assertRaisesRegex(saves.SaveError, 'CUSA02344'):
            saves.validate_backup(backup, expected_titles={'CUSA02344'})
        damaged = Path(self.temp.name) / 'damaged.zip'
        with zipfile.ZipFile(backup) as source, zipfile.ZipFile(damaged, 'w') as out:
            for info in source.infolist():
                data = source.read(info)
                out.writestr(info, data if info.filename == saves.MANIFEST else data[::-1] + b'!')
        with self.assertRaises(saves.SaveError):
            saves.validate_backup(damaged)
        plain = Path(self.temp.name) / 'plain.zip'
        with zipfile.ZipFile(plain, 'w') as archive:
            archive.writestr('USR-DATA', b'x')
        with self.assertRaisesRegex(saves.SaveError, 'not a save backup'):
            saves.validate_backup(plain)
        (Path(self.temp.name) / 'text.zip').write_text('hello')
        with self.assertRaises(saves.SaveError):
            saves.validate_backup(Path(self.temp.name) / 'text.zip')

    def test_restore_swaps_one_game_and_backs_up_first(self):
        backup = saves.create_backup(self.user, 'u1')
        make_save(self.user, 'SAVEDATA010', b'u1-game-CORRUPT')
        make_save(self.user, 'SAVEDATA011', b'u1-extra-slot')  # not in the backup: moved away
        make_save(self.user, 'SAVEDATA030', b'u3-game-v2')
        manifest, before = saves.restore_backup(self.user, backup)
        self.assertEqual(manifest['game'], 'u1')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-game-v1')
        self.assertFalse((self.user / 'savedata/1' / TITLE / 'SAVEDATA011').exists())
        self.assertEqual(usr(self.user, 'SAVEDATA030'), b'u3-game-v2')  # other games untouched
        self.assertEqual(usr(self.user, 'SAVEDATA0CC'), b'shared-v1')
        self.assertIn('before-restore', before.name)
        with zipfile.ZipFile(before) as archive:
            self.assertEqual(archive.read(f'savedata/1/{TITLE}/SAVEDATA010/USR-DATA'), b'u1-game-CORRUPT')
        leftovers = [p.name for p in self.user.iterdir() if p.name.startswith('.restore')]
        self.assertEqual(leftovers, [])

    def test_restore_rolls_back_when_a_rename_fails(self):
        backup = saves.create_backup(self.user, 'u1')
        make_save(self.user, 'SAVEDATA010', b'u1-game-v2')
        make_save(self.user, 'SAVEDATA01P', b'u1-profile-v2')
        calls = []

        def flaky(src, dst):
            calls.append((src, dst))
            if len(calls) == 4:  # after the first unit swapped and the second moved aside
                raise PermissionError('in use')
            os.rename(src, dst)
        with self.assertRaisesRegex(saves.SaveError, 'nothing was changed'):
            saves.restore_backup(self.user, backup, rename=flaky)
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-game-v2')
        self.assertEqual(usr(self.user, 'SAVEDATA01P'), b'u1-profile-v2')
        self.assertTrue((self.user / 'savedata/1' / TITLE / 'SAVEDATA010.sce_sys' / 'param.bin').exists())
        leftovers = [p.name for p in self.user.iterdir() if p.name.startswith('.restore')]
        self.assertEqual(leftovers, [])

    def test_restore_refused_while_the_game_runs(self):
        backup = saves.create_backup(self.user, 'u1')
        make_save(self.user, 'SAVEDATA010', b'u1-game-v2')
        with self.assertRaisesRegex(saves.SaveError, 'close the game'):
            saves.restore_backup(self.user, backup, game_running=lambda: True)
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-game-v2')

    def test_restore_all_merges_trophies(self):
        backup = saves.create_backup(self.user, 'all')
        (self.user / 'trophy' / 'NPWR09999_00.log').write_text('1 100\n2 200\n')
        saves.restore_backup(self.user, backup)
        text = (self.user / 'trophy' / 'NPWR09999_00.log').read_text()
        self.assertEqual(text, '1 100\n2 200\n')  # the trophy earned after the backup stays

    def test_auto_backup_skips_unchanged_and_keeps_newest(self):
        first = saves.auto_backup(self.user, keep=3, now=1_800_000_000)
        self.assertIsNotNone(first)
        self.assertIsNone(saves.auto_backup(self.user, keep=3, now=1_800_000_010))
        for step in range(5):
            make_save(self.user, 'SAVEDATA010', f'u1-game-{step}'.encode())
            saves.auto_backup(self.user, keep=3, now=1_800_000_100 + step * 100)
        autos = [b for b in saves.list_backups(self.user / 'backups') if b['kind'] == 'auto']
        self.assertEqual(len(autos), 3)
        with zipfile.ZipFile(autos[0]['path']) as archive:
            self.assertEqual(archive.read(f'savedata/1/{TITLE}/SAVEDATA010/USR-DATA'), b'u1-game-4')

    def test_import_and_delete(self):
        elsewhere = Path(self.temp.name) / 'other'
        backup = saves.create_backup(self.user, 'u3', dest=elsewhere)
        imported = saves.import_backup(backup, self.user / 'backups')
        self.assertEqual(imported.parent, self.user / 'backups')
        with self.assertRaises(saves.SaveError):
            saves.delete_backup(backup, self.user / 'backups')
        saves.delete_backup(imported, self.user / 'backups')
        self.assertFalse(imported.exists())

    def test_reads_canneli_v1_backups(self):
        path = Path(self.temp.name) / 'v1.zip'
        craft_zip(path, {f'savedata/1/{TITLE}/SAVEDATA010/USR-DATA': b'from-v1', 'trophy/notes.txt': b'?'},
                  {'format': saves.OLD_FORMAT, 'folders': ['savedata', 'trophy']})
        saves.restore_backup(self.user, path)
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'from-v1')


if __name__ == '__main__':
    unittest.main()
