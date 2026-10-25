"""Save slots (launcher/uncharted_slots.py): migration, swaps, store-back, recovery, slot management."""
from paths import ROOT
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import uncharted_saves as saves  # noqa: E402
import uncharted_slots as slots  # noqa: E402

TITLE = 'CUSA02320'


def param(subtitle):
    data = bytearray(1328)
    for offset, text in ((0, 'Uncharted'), (128, subtitle)):
        raw = text.encode()
        data[offset:offset + len(raw)] = raw
    struct.pack_into('<Iiq', data, 1280, 0, 0, 1792092050)
    return bytes(data)


def save_dir(user, name, title=TITLE):
    return Path(user) / 'savedata' / '1' / title / name


def make_save(user, name, content, subtitle='Campaign 2% Complete - The Search for El Dorado', title=TITLE):
    base = save_dir(user, name, title)
    base.mkdir(parents=True, exist_ok=True)
    (base / 'USR-DATA').write_bytes(content)
    meta = base.with_name(f'{name}.sce_sys')
    meta.mkdir(exist_ok=True)
    (meta / 'param.bin').write_bytes(param(subtitle))


def usr(user, name, title=TITLE):
    path = save_dir(user, name, title) / 'USR-DATA'
    return path.read_bytes() if path.exists() else None


def slot_usr(user, game, slot_id, name, title=TITLE):
    path = slots.slots_root(user) / game / slot_id / 'data' / 'savedata' / '1' / title / name / 'USR-DATA'
    return path.read_bytes() if path.exists() else None


def load_launcher():
    spec = importlib.util.spec_from_file_location('uncharted_launcher_slots', ROOT / 'launcher/uncharted_launcher.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SaveSlotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name) / 'user'
        make_save(self.user, 'SAVEDATA010', b'u1-main')
        make_save(self.user, 'SAVEDATA01P', b'u1-profile', 'Profile')
        make_save(self.user, 'SAVEDATA030', b'u3-main', 'Campaign 16% Complete - London Underground')
        make_save(self.user, 'SAVEDATA0CC', b'shared', 'Shared Profile')

    def tearDown(self):
        self.temp.cleanup()

    def names(self, game):
        return {s['id']: s for s in slots.list_slots(self.user, game)}

    # ---- first run -------------------------------------------------------------------------------
    def test_first_run_turns_the_save_into_main_without_moving_it(self):
        found = slots.list_slots(self.user, 'u3')
        self.assertEqual([(s['id'], s['name'], s['in_place'], s['empty']) for s in found],
                         [('main', 'Main', True, False)])
        self.assertEqual(found[0]['subtitle'], 'Campaign 16% Complete - London Underground')
        self.assertEqual(usr(self.user, 'SAVEDATA030'), b'u3-main')
        self.assertEqual(slot_usr(self.user, 'u3', 'main', 'SAVEDATA030'), b'u3-main')
        self.assertTrue((slots.slots_root(self.user) / 'u3' / 'main' / 'data' / 'savedata' / '1' / TITLE /
                         'SAVEDATA030.sce_sys' / 'param.bin').is_file())
        # a game without a save gets an empty Main
        self.assertTrue(slots.list_slots(self.user, 'u2')[0]['empty'])

    def test_launches_make_no_slots_for_games_without_saves(self):
        slots.sync(self.user, {})
        slots.store_back_all(self.user)
        slots.recover(self.user)
        self.assertEqual(sorted(p.name for p in slots.slots_root(self.user).iterdir() if p.is_dir()), ['u1', 'u3'])

    def test_profiles_never_go_into_slots(self):
        slots.list_slots(self.user, 'u1')
        held = list((slots.slots_root(self.user) / 'u1' / 'main' / 'data' / 'savedata' / '1' / TITLE).iterdir())
        self.assertEqual(sorted(p.name for p in held), ['SAVEDATA010', 'SAVEDATA010.sce_sys'])

    # ---- swaps -----------------------------------------------------------------------------------
    def test_swap_in_and_out_keeps_both_saves(self):
        crushing = slots.create_slot(self.user, 'u1', 'Crushing run')
        self.assertTrue(slots.activate(self.user, 'u1', crushing))
        self.assertIsNone(usr(self.user, 'SAVEDATA010'))          # empty slot: the game starts anew
        self.assertEqual(usr(self.user, 'SAVEDATA01P'), b'u1-profile')  # profile stays
        self.assertEqual(slots.in_place(self.user, 'u1'), crushing)
        make_save(self.user, 'SAVEDATA010', b'u1-crushing')          # the game saves
        self.assertTrue(slots.activate(self.user, 'u1', 'main'))
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')
        self.assertEqual(slot_usr(self.user, 'u1', crushing, 'SAVEDATA010'), b'u1-crushing')
        self.assertTrue(slots.activate(self.user, 'u1', crushing))
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-crushing')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')
        self.assertFalse(slots.activate(self.user, 'u1', crushing))  # already in place

    def test_failed_swap_rolls_back(self):
        other = slots.create_slot(self.user, 'u1', 'Other', copy_from='main')
        make_save(self.user, 'SAVEDATA010', b'u1-main-v2')
        calls = []

        def flaky(source, target):
            calls.append(source)
            if len(calls) == 2:  # the first staged folder moving in
                raise OSError('disk busy')
            Path(source).rename(target)
        with self.assertRaises(slots.SlotError):
            slots.activate(self.user, 'u1', other, rename=flaky)
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main-v2')
        self.assertEqual(slots.in_place(self.user, 'u1'), 'main')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main-v2')  # stored first
        self.assertEqual(slot_usr(self.user, 'u1', other, 'SAVEDATA010'), b'u1-main')
        self.assertEqual([p.name for p in self.user.iterdir() if p.name.startswith('.slot')], [])

    def test_swaps_and_slot_copies_cover_every_title_folder(self):
        make_save(self.user, 'SAVEDATA010', b'u1-eu', title='CUSA02343')
        other = slots.create_slot(self.user, 'u1', 'Other')
        slots.activate(self.user, 'u1', other)
        self.assertIsNone(usr(self.user, 'SAVEDATA010', 'CUSA02343'))
        slots.activate(self.user, 'u1', 'main')
        self.assertEqual(usr(self.user, 'SAVEDATA010', 'CUSA02343'), b'u1-eu')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')

    # ---- store back and recovery -----------------------------------------------------------------
    def test_store_back_after_exit_goes_into_the_slot_in_place_only(self):
        run = slots.create_slot(self.user, 'u1', 'Run 2', copy_from='main')
        slots.activate(self.user, 'u1', run)
        make_save(self.user, 'SAVEDATA010', b'u1-run2-progress', 'Campaign 40% Complete - Fortress')
        self.assertEqual(slots.store_back_all(self.user), {})
        self.assertEqual(slot_usr(self.user, 'u1', run, 'SAVEDATA010'), b'u1-run2-progress')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')
        self.assertEqual(self.names('u1')[run]['subtitle'], 'Campaign 40% Complete - Fortress')
        self.assertFalse(slots.store_back(self.user, 'u1'))  # nothing new

    def test_store_back_is_allowed_while_a_game_runs(self):
        slots.list_slots(self.user, 'u1')
        make_save(self.user, 'SAVEDATA010', b'u1-live')
        self.assertTrue(slots.store_back(self.user, 'u1'))
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-live')

    def test_recovery_after_unclean_exit_stores_before_swapping(self):
        run = slots.create_slot(self.user, 'u1', 'Run 2')
        slots.activate(self.user, 'u1', run)
        # the launcher closed (or crashed) while the game ran; the game saved into the folder
        make_save(self.user, 'SAVEDATA010', b'u1-run2-unseen')
        self.assertEqual(slots.recover(self.user, running=lambda: False), {})
        self.assertEqual(slot_usr(self.user, 'u1', run, 'SAVEDATA010'), b'u1-run2-unseen')
        # even without recover(), the next swap stores the save in place into its own slot first
        make_save(self.user, 'SAVEDATA010', b'u1-run2-later')
        slots.sync(self.user, {'u1': 'main'})
        self.assertEqual(slot_usr(self.user, 'u1', run, 'SAVEDATA010'), b'u1-run2-later')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')

    def test_swap_cut_short_after_the_folders_moved(self):
        other = slots.create_slot(self.user, 'u1', 'Other')
        make_save(self.user, 'SAVEDATA010', b'u1-main-v2')
        real = slots._write_state

        def crash_on_final(user, game, state):
            if state['in_place'] == other:
                raise KeyboardInterrupt  # the process died before the last write
            real(user, game, state)
        with mock.patch.object(slots, '_write_state', crash_on_final), self.assertRaises(KeyboardInterrupt):
            slots.activate(self.user, 'u1', other)
        self.assertEqual(slots._state(self.user, 'u1')['pending'], other)
        slots.recover(self.user, running=lambda: False)
        self.assertEqual(slots.in_place(self.user, 'u1'), other)
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main-v2')
        self.assertIsNone(slot_usr(self.user, 'u1', other, 'SAVEDATA010'))

    def test_swap_cut_short_halfway_never_crosses_slots(self):
        other = slots.create_slot(self.user, 'u1', 'Other')
        slots.activate(self.user, 'u1', other)
        make_save(self.user, 'SAVEDATA010', b'u1-other-a')
        make_save(self.user, 'SAVEDATA010', b'u1-other-b', title='CUSA02343')
        slots.store_back(self.user, 'u1')
        calls = []

        def die(source, target):
            calls.append(source)
            Path(source).rename(target)
            if len(calls) == 2:  # one title folder swapped, the rest not
                raise KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            slots.activate(self.user, 'u1', 'main', rename=die)
        self.assertEqual(slots.recover(self.user, running=lambda: True), {})  # waits while a game runs
        self.assertEqual(slots._state(self.user, 'u1')['pending'], 'main')
        slots.recover(self.user, running=lambda: False)
        self.assertEqual(slots.in_place(self.user, 'u1'), 'main')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')
        self.assertIsNone(usr(self.user, 'SAVEDATA010', 'CUSA02343'))
        self.assertEqual(slot_usr(self.user, 'u1', other, 'SAVEDATA010'), b'u1-other-a')
        self.assertEqual(slot_usr(self.user, 'u1', other, 'SAVEDATA010', 'CUSA02343'), b'u1-other-b')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')

    def test_lost_state_gets_a_new_slot_instead_of_overwriting(self):
        other = slots.create_slot(self.user, 'u1', 'Other')
        slots.activate(self.user, 'u1', other)
        make_save(self.user, 'SAVEDATA010', b'u1-who-knows')
        (slots.slots_root(self.user) / 'u1' / 'state.json').unlink()
        found = self.names('u1')
        self.assertEqual(len(found), 3)
        new = next(s for s in found.values() if s['in_place'])
        self.assertNotIn(new['id'], ('main', other))
        self.assertEqual(new['name'], 'Main 2')
        self.assertEqual(slot_usr(self.user, 'u1', new['id'], 'SAVEDATA010'), b'u1-who-knows')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')
        self.assertIsNone(slot_usr(self.user, 'u1', other, 'SAVEDATA010'))

    # ---- running guard ---------------------------------------------------------------------------
    def test_swaps_are_refused_while_a_game_runs(self):
        other = slots.create_slot(self.user, 'u1', 'Other')
        with self.assertRaises(slots.SlotError) as caught:
            slots.activate(self.user, 'u1', other, running=lambda: True)
        self.assertEqual(caught.exception.code, 'running')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')
        self.assertEqual(slots.in_place(self.user, 'u1'), 'main')
        self.assertFalse(slots.activate(self.user, 'u1', 'main', running=lambda: True))  # no swap needed
        with self.assertRaises(slots.SlotError):
            slots.sync(self.user, {'u1': other}, running=lambda: True)

    # ---- managing slots --------------------------------------------------------------------------
    def test_rename_duplicate_delete(self):
        copy = slots.duplicate_slot(self.user, 'u1', 'main', 'Main (copy)')
        self.assertEqual(slot_usr(self.user, 'u1', copy, 'SAVEDATA010'), b'u1-main')
        self.assertEqual(self.names('u1')[copy]['subtitle'], 'Campaign 2% Complete - The Search for El Dorado')
        self.assertEqual(slots.rename_slot(self.user, 'u1', copy, '  Chapter   5  replay '), 'Chapter 5 replay')
        with self.assertRaises(slots.SlotError) as caught:
            slots.rename_slot(self.user, 'u1', copy, 'main')
        self.assertEqual(caught.exception.code, 'name-taken')
        with self.assertRaises(slots.SlotError) as caught:
            slots.create_slot(self.user, 'u1', '   ')
        self.assertEqual(caught.exception.code, 'name-empty')
        with self.assertRaises(slots.SlotError) as caught:
            slots.delete_slot(self.user, 'u1', 'main')
        self.assertEqual(caught.exception.code, 'active')
        with self.assertRaises(slots.SlotError):
            slots.delete_slot(self.user, 'u1', copy, active=copy)  # chosen on the Play page
        slots.delete_slot(self.user, 'u1', copy)
        self.assertEqual(list(self.names('u1')), ['main'])
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')

    def test_copy_of_the_slot_in_place_takes_the_newest_save(self):
        slots.list_slots(self.user, 'u1')
        make_save(self.user, 'SAVEDATA010', b'u1-main-newest')
        copy = slots.create_slot(self.user, 'u1', 'Copy', copy_from='main')
        self.assertEqual(slot_usr(self.user, 'u1', copy, 'SAVEDATA010'), b'u1-main-newest')

    def test_slot_ids_cannot_leave_the_slots_folder(self):
        for bad in ('../u2', '..', 'a/b', ''):
            with self.assertRaises(slots.SlotError):
                slots.activate(self.user, 'u1', bad)

    def test_restore_goes_into_the_active_slot(self):
        backup = saves.create_backup(self.user, 'u1', kind='manual')
        other = slots.create_slot(self.user, 'u1', 'Other')
        slots.sync(self.user, {'u1': other})
        saves.restore_backup(self.user, backup)
        slots.store_back_all(self.user)
        self.assertEqual(slot_usr(self.user, 'u1', other, 'SAVEDATA010'), b'u1-main')
        self.assertEqual(slots.in_place(self.user, 'u1'), other)

    # ---- backups of slots ------------------------------------------------------------------------
    def zip_names(self, path):
        import zipfile
        with zipfile.ZipFile(path) as archive:
            return set(archive.namelist())

    def test_backups_hold_every_slot(self):
        other = slots.create_slot(self.user, 'u1', 'Other', copy_from='main')
        slots.list_slots(self.user, 'u3')
        manual = self.zip_names(saves.create_backup(self.user, 'u1'))
        for slot_id in ('main', other):
            self.assertIn(f'save_slots/u1/{slot_id}/slot.json', manual)
            self.assertIn(f'save_slots/u1/{slot_id}/data/savedata/1/{TITLE}/SAVEDATA010/USR-DATA', manual)
        self.assertFalse(any(n.startswith('save_slots/u3/') for n in manual))  # u1's backup: u1's slots
        self.assertFalse(any(n.endswith(('state.json', '.lock')) for n in manual))
        auto = self.zip_names(saves.auto_backup(self.user))
        self.assertIn(f'save_slots/u1/{other}/slot.json', auto)
        self.assertIn('save_slots/u3/main/slot.json', auto)
        saves.validate_backup(saves.list_backups(saves.backup_dir(self.user))[0]['path'])

    def test_delete_backs_the_slot_up_and_restore_brings_it_back(self):
        run = slots.create_slot(self.user, 'u1', 'Run 2')
        slots.activate(self.user, 'u1', run)
        make_save(self.user, 'SAVEDATA010', b'u1-run2')
        slots.activate(self.user, 'u1', 'main')
        made = slots.delete_slot(self.user, 'u1', run)
        self.assertEqual(saves.read_manifest(made)['kind'], 'before-delete')
        self.assertNotIn(run, self.names('u1'))
        make_save(self.user, 'SAVEDATA010', b'u1-main-v2')
        saves.restore_backup(self.user, made)
        found = self.names('u1')
        self.assertEqual(found[run]['name'], 'Run 2')
        self.assertEqual(slot_usr(self.user, 'u1', run, 'SAVEDATA010'), b'u1-run2')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main-v2')  # the save in place is untouched
        self.assertEqual(slots.in_place(self.user, 'u1'), 'main')

    def test_restore_never_overwrites_existing_slots(self):
        other = slots.create_slot(self.user, 'u1', 'Other', copy_from='main')
        backup = saves.create_backup(self.user, 'all')
        slots.activate(self.user, 'u1', other)
        make_save(self.user, 'SAVEDATA010', b'u1-other-new')
        slots.store_back(self.user, 'u1')
        saves.restore_backup(self.user, backup)
        self.assertEqual(slot_usr(self.user, 'u1', other, 'SAVEDATA010'), b'u1-other-new')
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')  # the backup's save, in place
        slots.store_back_all(self.user)                             # goes into the active slot
        self.assertEqual(slot_usr(self.user, 'u1', other, 'SAVEDATA010'), b'u1-main')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')

    def test_a_backup_cannot_smuggle_another_games_slot(self):
        import hashlib
        import json
        import zipfile
        path = Path(self.temp.name) / 'bad.zip'
        name = 'save_slots/u2/main/slot.json'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr(name, '{}')
            archive.writestr(saves.MANIFEST, json.dumps({
                'format': saves.FORMAT, 'game': 'u1',
                'files': [{'path': name, 'size': 2, 'sha256': hashlib.sha256(b'{}').hexdigest()}]}))
        with self.assertRaises(saves.SaveError):
            saves.validate_backup(path)

    # ---- slot ids are never used twice -----------------------------------------------------------
    def test_a_deleted_slots_id_is_never_given_again(self):
        first = slots.create_slot(self.user, 'u1', 'First')
        slots.delete_slot(self.user, 'u1', first)
        second = slots.create_slot(self.user, 'u1', 'Second')
        self.assertNotEqual(first, second)
        slots.delete_slot(self.user, 'u1', second)
        self.assertNotIn(slots.create_slot(self.user, 'u1', 'Third'), (first, second))
        # a lost state.json cannot take the numbers back: the slots still on disk set the floor
        keep = slots.create_slot(self.user, 'u1', 'Keep')
        (slots.slots_root(self.user) / 'u1' / 'state.json').unlink()
        self.assertGreater(int(slots.create_slot(self.user, 'u1', 'After')[4:]), int(keep[4:]))

    def test_deleting_main_does_not_free_its_id_either(self):
        other = slots.create_slot(self.user, 'u1', 'Other')
        slots.activate(self.user, 'u1', other)
        slots.delete_slot(self.user, 'u1', 'main')
        state = slots._state(self.user, 'u1')
        self.assertEqual(state['counter'], int(other[4:]))
        state.update(in_place='gone')      # the slot in place vanished: a new one is made for the save
        slots._write_json(slots.slots_root(self.user) / 'u1' / 'state.json', state)
        ids = [s['id'] for s in slots.list_slots(self.user, 'u1')]
        self.assertNotIn('main', ids)
        self.assertEqual(len(ids), 2)

    def test_restoring_a_deleted_slot_after_new_slots_were_made(self):
        run = slots.create_slot(self.user, 'u1', 'Run 2')
        slots.activate(self.user, 'u1', run)
        make_save(self.user, 'SAVEDATA010', b'u1-run2')
        slots.activate(self.user, 'u1', 'main')
        made = slots.delete_slot(self.user, 'u1', run)
        newer = slots.create_slot(self.user, 'u1', 'Newer', copy_from='main')
        self.assertNotEqual(newer, run)
        done, _before = saves.restore_backup(self.user, made)
        self.assertEqual(done['slots_renamed'], [])
        self.assertEqual(slot_usr(self.user, 'u1', run, 'SAVEDATA010'), b'u1-run2')
        self.assertEqual(slot_usr(self.user, 'u1', newer, 'SAVEDATA010'), b'u1-main')
        self.assertEqual(self.names('u1')[run]['name'], 'Run 2')

    def test_restore_meets_an_existing_slot_id_and_keeps_both(self):
        other = slots.create_slot(self.user, 'u1', 'Other', copy_from='main')
        backup = saves.create_backup(self.user, 'u1')
        slots.activate(self.user, 'u1', other)
        make_save(self.user, 'SAVEDATA010', b'u1-other-new')
        slots.store_back(self.user, 'u1')
        done, _before = saves.restore_backup(self.user, backup)
        self.assertEqual(len(done['slots_renamed']), 1)
        moved = done['slots_renamed'][0]
        self.assertEqual((moved['game'], moved['was'], moved['name']), ('u1', other, 'Other (restored)'))
        self.assertNotIn(moved['id'], ('main', other))
        found = self.names('u1')
        self.assertEqual(found[moved['id']]['name'], 'Other (restored)')
        self.assertEqual(slots._manifest(self.user, 'u1', moved['id'])['id'], moved['id'])
        self.assertEqual(slot_usr(self.user, 'u1', moved['id'], 'SAVEDATA010'), b'u1-main')   # the backup's
        self.assertEqual(done['slots_unchanged'], ['Main'])   # same content: nothing to add
        self.assertEqual(sorted(found), sorted(['main', other, moved['id']]))
        # restoring again names the second copy differently
        again, _before = saves.restore_backup(self.user, backup)
        self.assertEqual(again['slots_renamed'][0]['name'], 'Other (restored 2)')

    # ---- backups made before deleting a slot are not pruned by "backups to keep" -----------------
    def test_prune_keeps_every_before_delete_backup_up_to_the_generous_cap(self):
        made = []
        for number in range(4):
            slot = slots.create_slot(self.user, 'u1', f'Gone {number}')
            made.append(slots.delete_slot(self.user, 'u1', slot, now=1_700_000_000 + number * 60))
        dest = saves.backup_dir(self.user)
        self.assertEqual(saves.prune(dest, 1, kind='before-delete'), [])
        self.assertTrue(all(path.is_file() for path in made))
        with mock.patch.object(saves, 'DELETE_BACKUPS_KEEP', 2):
            self.assertEqual(sorted(saves.prune(dest, 1, kind='before-delete')), sorted(made[:2]))   # oldest first
        self.assertTrue(made[2].is_file() and made[3].is_file())

    def test_other_kinds_still_follow_the_keep_count(self):
        for number in range(3):
            saves.create_backup(self.user, 'u1', kind='before-edit', now=1_700_000_000 + number * 60)
        self.assertEqual(len(saves.prune(saves.backup_dir(self.user), 1, kind='before-edit')), 2)

    # ---- first-time setup leaves no empty slot behind --------------------------------------------
    def test_a_crash_while_making_main_leaves_no_slot_and_the_next_call_does_it(self):
        with mock.patch.object(slots, '_copy_units', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            slots.list_slots(self.user, 'u1')
        self.assertEqual(slots._slot_ids(self.user, 'u1'), [])
        with mock.patch.object(slots, '_publish', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            slots.list_slots(self.user, 'u1')
        self.assertEqual(slots._slot_ids(self.user, 'u1'), [])
        found = slots.list_slots(self.user, 'u1')
        self.assertEqual([(s['id'], s['name'], s['empty']) for s in found], [('main', 'Main', False)])
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')
        hidden = [p.name for p in (slots.slots_root(self.user) / 'u1').iterdir() if p.name.startswith('.new')]
        self.assertEqual(hidden, [])

    def test_a_crash_while_copying_a_slot_leaves_no_half_slot(self):
        slots.list_slots(self.user, 'u1')
        with mock.patch.object(slots.shutil, 'copytree', side_effect=OSError('disk full')), self.assertRaises(OSError):
            slots.create_slot(self.user, 'u1', 'Copy', copy_from='main')
        self.assertEqual(list(self.names('u1')), ['main'])

    # ---- the lock --------------------------------------------------------------------------------
    def lock_file(self):
        return slots.slots_root(self.user) / '.lock'

    def test_the_lock_holds_our_pid_and_goes_away(self):
        with slots._lock(self.user):
            self.assertEqual(self.lock_file().read_text(), str(os.getpid()))
        self.assertFalse(self.lock_file().exists())

    def test_a_lock_of_a_dead_process_is_taken_over_at_once(self):
        child = subprocess.Popen([sys.executable, '-c', 'pass'])
        child.wait()
        self.lock_file().parent.mkdir(parents=True, exist_ok=True)
        self.lock_file().write_text(str(child.pid))   # fresh, but its owner is gone
        with mock.patch.object(slots, 'LOCK_WAIT', 0):
            with slots._lock(self.user):
                self.assertEqual(self.lock_file().read_text(), str(os.getpid()))

    def test_a_lock_of_a_live_process_is_respected(self):
        self.lock_file().parent.mkdir(parents=True, exist_ok=True)
        self.lock_file().write_text(str(os.getpid()))
        with mock.patch.object(slots, 'LOCK_WAIT', 0.2), self.assertRaises(slots.SlotError):
            with slots._lock(self.user):
                self.fail('the lock was not ours to take')
        self.assertEqual(self.lock_file().read_text(), str(os.getpid()))
        self.lock_file().unlink()

    def test_an_old_lock_is_ignored_even_when_its_pid_is_alive(self):
        self.lock_file().parent.mkdir(parents=True, exist_ok=True)
        self.lock_file().write_text(str(os.getpid()))
        old = self.lock_file().stat().st_mtime - slots.LOCK_STALE - 5
        os.utime(self.lock_file(), (old, old))
        with mock.patch.object(slots, 'LOCK_WAIT', 0):
            with slots._lock(self.user):
                pass

    def test_a_lock_somebody_else_took_over_is_not_removed(self):
        with slots._lock(self.user):
            self.lock_file().write_text('4000000000')   # another launcher's lock
        self.assertEqual(self.lock_file().read_text(), '4000000000')

    # ---- is a game running? ----------------------------------------------------------------------
    @unittest.skipUnless(os.name == 'nt', 'tasklist is Windows only')
    def test_game_process_running_is_true_when_tasklist_fails(self):
        def result(code, out='', err=''):
            return subprocess.CompletedProcess([], code, out, err)
        for failure in (mock.Mock(side_effect=FileNotFoundError('tasklist')),
                        mock.Mock(side_effect=subprocess.TimeoutExpired('tasklist', 10)),
                        mock.Mock(return_value=result(1, '', 'Access is denied'))):
            with mock.patch.object(saves.subprocess, 'run', failure), \
                    self.assertLogs('uncharted.saves', 'WARNING') as logged:
                self.assertTrue(saves.game_process_running())
            self.assertIn('tasklist', logged.output[0])
        with mock.patch.object(saves.subprocess, 'run', return_value=result(0, 'INFO: No tasks are running.')):
            self.assertFalse(saves.game_process_running())
        with mock.patch.object(saves.subprocess, 'run', return_value=result(0, '"bb-probe.exe","12","Console"')):
            self.assertTrue(saves.game_process_running())

    # ---- names ------------------------------------------------------------------------------------
    def test_names_are_compared_as_shown_too(self):
        def shown(name):
            return 'Hauptslot' if name == 'Main' else name
        with self.assertRaises(slots.SlotError) as caught:
            slots.create_slot(self.user, 'u1', 'hauptslot', shown=shown)
        self.assertEqual(caught.exception.code, 'name-taken')
        with self.assertRaises(slots.SlotError):
            slots.create_slot(self.user, 'u1', 'MAIN', shown=shown)    # the stored name still counts
        other = slots.create_slot(self.user, 'u1', 'Other', shown=shown)
        with self.assertRaises(slots.SlotError):
            slots.rename_slot(self.user, 'u1', other, 'Hauptslot', shown=shown)
        with self.assertRaises(slots.SlotError):
            slots.duplicate_slot(self.user, 'u1', 'main', 'Hauptslot', shown=shown)
        self.assertEqual(slots.rename_slot(self.user, 'u1', other, 'Other', shown=shown), 'Other')   # itself is fine
        self.assertTrue(slots.create_slot(self.user, 'u1', 'Hauptslot'))   # no translation in play: fine

    def test_the_launcher_names_the_slots_a_restore_replaces(self):
        launcher = load_launcher()
        other = slots.create_slot(self.user, 'u1', 'Crushing run')
        cls = launcher.Launcher
        fake = mock.Mock()
        fake.slot_list = lambda game: slots.list_slots(self.user, game)
        fake.active_slot = lambda game, found: other if game == 'u1' else next(s['id'] for s in found if s['in_place'])
        fake.slot_name = lambda slot: cls.slot_name(fake, slot)
        self.assertEqual(cls.restore_replaces(fake, {'format': saves.FORMAT, 'game': 'u1'}),
                         "Drake's Fortune: Crushing run")
        both = cls.restore_replaces(fake, {'format': saves.FORMAT, 'game': 'all'})
        self.assertTrue(both.startswith("Drake's Fortune: Crushing run, Among Thieves: Main"))
        self.assertEqual(cls.restore_replaces(fake, {'format': saves.FORMAT, 'game': 'shared'}), '')

    # ---- the launcher's launch paths -------------------------------------------------------------
    def test_empty_slot_launch_path_and_store_back(self):
        launcher = load_launcher()
        fresh = slots.create_slot(self.user, 'u1', 'Fresh')
        settings = {**launcher.APP_DEFAULTS, 'user_dir': str(self.user), 'start': 'u1', 'auto_backup': False,
                    'save_slots': {'u1': fresh}}
        user = self.user

        class FakeGame:
            """Stands in for run.py: no game starts; it 'saves' a new campaign when it ends."""
            def __init__(self, *_args, **_kwargs):
                assert usr(user, 'SAVEDATA010') is None   # the empty slot is in place
                assert usr(user, 'SAVEDATA01P') == b'u1-profile'
                self.stdout = io.BytesIO(b'game output\n')

            def wait(self):
                make_save(user, 'SAVEDATA010', b'u1-fresh-campaign', 'Campaign 1% Complete')
                return 0
        with mock.patch.object(launcher, 'game_environment', return_value={}), \
                mock.patch.object(launcher.uncharted_saves, 'game_process_running', return_value=False), \
                mock.patch.object(launcher.subprocess, 'Popen', FakeGame), \
                mock.patch.object(launcher, 'attach_stdio'):
            self.assertEqual(launcher.play_without_window(settings), 0)
        self.assertEqual(slot_usr(self.user, 'u1', fresh, 'SAVEDATA010'), b'u1-fresh-campaign')
        self.assertEqual(slot_usr(self.user, 'u1', 'main', 'SAVEDATA010'), b'u1-main')
        found = self.names('u1')
        self.assertTrue(found[fresh]['last_played'])
        self.assertFalse(found['main']['last_played'])
        log = (self.user / 'last_run.log').read_text(encoding='utf-8')
        self.assertIn(f'Save slot swapped in: u1 {fresh}\ngame output\nSave stored in slot: u1 {fresh}\n', log)

    def test_launch_path_refuses_a_swap_while_a_game_runs(self):
        launcher = load_launcher()
        other = slots.create_slot(self.user, 'u1', 'Other')
        settings = {**launcher.APP_DEFAULTS, 'user_dir': str(self.user), 'start': 'u1', 'auto_backup': False,
                    'save_slots': {'u1': other}}
        popen = mock.Mock()
        with mock.patch.object(launcher, 'game_environment', return_value={}), \
                mock.patch.object(launcher.uncharted_saves, 'game_process_running', return_value=True), \
                mock.patch.object(launcher.subprocess, 'Popen', popen), \
                mock.patch.object(launcher, 'attach_stdio'), mock.patch.object(launcher.sys, 'stderr', io.StringIO()):
            self.assertEqual(launcher.play_without_window(settings), 1)
        popen.assert_not_called()
        self.assertEqual(usr(self.user, 'SAVEDATA010'), b'u1-main')


if __name__ == '__main__':
    unittest.main()
