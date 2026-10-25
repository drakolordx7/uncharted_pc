# SPDX-License-Identifier: GPL-2.0-or-later
"""Save slots: named copies of each game's campaign save, swapped in before a game starts.

Each game keeps exactly one autosave (SAVEDATA010/020/030 with its .sce_sys, see uncharted_saves); the
profiles (SAVEDATA0xP, SAVEDATA0CC: finished difficulties, rewards, settings) stay shared. A slot holds
one game's autosave folders, as they lie under the user folder:
    <user>/save_slots/<game>/<slot id>/slot.json       name, created, last played, the game's texts
    <user>/save_slots/<game>/<slot id>/data/savedata/<user id>/<title id>/SAVEDATA0x0[.sce_sys]
    <user>/save_slots/<game>/state.json                {"in_place": slot id, "pending": slot id or null,
                                                        "counter": the highest slot number ever given}
Slot ids are never used twice: a new id is "slot" + the next number of the counter, so restoring the
backup made when a slot was deleted can never meet another slot with its id.
The save in the saves folder always belongs to the slot `in_place` names, so it can be stored back at
any time (after the game exits, or at the next start when the launcher was closed or crashed meanwhile);
storing back only copies, so it is safe while a game runs. Swapping moves folders and is refused while
a game runs: the save in place is stored back into its slot first, the other slot's copy is staged next
to the saves, `pending` is written, then uncharted_saves.swap_units renames (rolling back on failure),
and only then `in_place` changes. A swap cut short leaves `pending`; the next call compares the save in
place with both slots and finishes or undoes it, so a slot is never overwritten with another one's save.
The first call for a game turns the existing save into the slot "Main" (copied, nothing moves). A new
slot is built in a hidden folder and renamed in, so a crash leaves no half-made slot.
"""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import time

import uncharted_saves as saves

GAMES = ('u1', 'u2', 'u3')
GAME_SAVE = {'u1': 'SAVEDATA010', 'u2': 'SAVEDATA020', 'u3': 'SAVEDATA030'}
DEFAULT_ID, DEFAULT_NAME = 'main', 'Main'
MAX_NAME = 40
ID_RE = re.compile(r'^[a-z0-9]{1,24}$')
LOCK_STALE = 120  # seconds after which a lock is ignored even when its process seems alive (PID reused)
LOCK_WAIT = 5     # seconds to wait for another slot change before giving up


class SlotError(saves.SaveError):
    """A slot change that cannot be done. `code`: 'running', 'name-empty', 'name-taken', 'active',
    'missing' or '' (the message says what went wrong)."""

    def __init__(self, message, code=''):
        super().__init__(message)
        self.code = code


# ---- where things are ------------------------------------------------------------------------

def slots_root(user_dir):
    return Path(user_dir) / 'save_slots'


def _game_dir(user_dir, game):
    if game not in GAMES:
        raise SlotError(f'unknown game {game!r}')
    return slots_root(user_dir) / game


def _slot_dir(user_dir, game, slot_id):
    if not ID_RE.match(str(slot_id or '')):
        raise SlotError('this save slot no longer exists', 'missing')
    return _game_dir(user_dir, game) / slot_id


def game_units(user_dir, game):
    """The game's autosave in the saves folder: relative POSIX paths of SAVEDATA0x0 and its .sce_sys
    in every user id and title id folder."""
    name = GAME_SAVE[game]
    found = []
    for user, title in saves._title_dirs(user_dir):
        for entry in (name, f'{name}.sce_sys'):
            if (title / entry).is_dir():
                found.append(f'savedata/{user.name}/{title.name}/{entry}')
    return found


def _data_units(data):
    """The units a slot's data folder holds (same relative paths as in the user folder)."""
    found = []
    root = Path(data) / 'savedata'
    if not root.is_dir():
        return found
    for user in sorted(root.iterdir()):
        if not user.is_dir():
            continue
        for title in sorted(user.iterdir()):
            if not title.is_dir():
                continue
            for entry in sorted(title.iterdir()):
                if entry.is_dir() and saves.NAME_RE.match(entry.name):
                    found.append(f'savedata/{user.name}/{title.name}/{entry.name}')
    return found


def _digest(root, unit_list):
    """(relative file, size, SHA-256) of every file of the units: equal digests, equal saves."""
    found = []
    for rel, path in saves._files_of(root, unit_list):
        found.append((rel, path.stat().st_size, saves._sha256(path)))
    return sorted(found)


def _newest(root, unit_list):
    newest = 0.0
    for unit in unit_list:
        newest = max(newest, saves._tree_stats(Path(root).joinpath(*unit.split('/')))[0])
    return newest or None


def _texts(root, unit_list):
    """The game's own texts (param.bin) of the first .sce_sys among the units."""
    for unit in unit_list:
        if unit.endswith('.sce_sys'):
            param = saves.read_param(Path(root).joinpath(*unit.split('/')))
            if param:
                return {'title': param[0], 'subtitle': param[1], 'detail': param[2]}
    return {'title': '', 'subtitle': '', 'detail': ''}


def _read_json(path, default=None):
    try:
        value = json.loads(Path(path).read_text(encoding='utf-8'))
        return value if isinstance(value, dict) else default
    except (OSError, ValueError):
        return default


def _write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f'.{path.name}.tmp')
    temporary.write_text(json.dumps(value, indent=2), encoding='utf-8')
    os.replace(temporary, path)


def _copy_units(source_root, unit_list, dest_root):
    for unit in unit_list:
        shutil.copytree(Path(source_root).joinpath(*unit.split('/')), Path(dest_root).joinpath(*unit.split('/')))


def _pid_alive(pid):
    """Whether a process with this id exists (never signals it: on Windows os.kill would end it)."""
    if pid <= 0:
        return False
    if os.name == 'nt':
        import ctypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.get_last_error() == 5  # access denied: it exists; 87: no such process
        try:
            code = ctypes.c_ulong()
            if kernel.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(code)):
                return code.value == 259  # STILL_ACTIVE
            return True
        finally:
            kernel.CloseHandle(ctypes.c_void_p(handle))
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True
    return True


def _lock_owner(path):
    """The process id a lock file names, or None when it is empty or unreadable."""
    try:
        text = Path(path).read_text(encoding='ascii').strip()
        return int(text) if text.isdigit() else None
    except (OSError, ValueError):
        return None


def _lock_is_stale(path):
    owner = _lock_owner(path)
    if owner is not None and not _pid_alive(owner):
        return True  # the launcher that took it is gone
    try:
        return time.time() - Path(path).stat().st_mtime > LOCK_STALE
    except OSError:
        return False


@contextmanager
def _lock(user_dir):
    """One slot change at a time (the launcher window and a --play shortcut could overlap). The lock
    file holds the owner's process id: a lock whose process is gone is taken over at once."""
    path = slots_root(user_dir) / '.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    mine = str(os.getpid())
    deadline = time.time() + LOCK_WAIT
    while True:
        try:
            handle = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            try:
                os.write(handle, mine.encode())
            finally:
                os.close(handle)
            break
        except FileExistsError:
            if _lock_is_stale(path) and _lock_is_stale(path):  # twice: a lock taken over meanwhile is fresh
                try:
                    path.unlink()
                except OSError:
                    pass
                continue
            if time.time() > deadline:
                raise SlotError('the save slots are busy, try again in a moment')
            time.sleep(0.05)
    try:
        yield
    finally:
        if _lock_owner(path) == os.getpid():  # never remove a lock somebody else took over
            try:
                path.unlink()
            except OSError:
                pass


# ---- slot records ----------------------------------------------------------------------------

def _stamp(now=None):
    return time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(now))


def _manifest(user_dir, game, slot_id):
    return _read_json(_slot_dir(user_dir, game, slot_id) / 'slot.json')


def _slot_ids(user_dir, game):
    folder = _game_dir(user_dir, game)
    if not folder.is_dir():
        return []
    return sorted(p.name for p in folder.iterdir()
                  if p.is_dir() and ID_RE.match(p.name) and (p / 'slot.json').is_file())


def _fix_data(slot):
    """Finishes a replacement of a slot's data cut short (see _store)."""
    data, old, new = slot / 'data', slot / '.data-old', slot / '.data-new'
    if not data.exists() and old.exists():
        os.rename(old, data)
    shutil.rmtree(old, ignore_errors=True)
    shutil.rmtree(new, ignore_errors=True)
    data.mkdir(parents=True, exist_ok=True)


def _counter(user_dir, game):
    """The highest slot number ever given out (the first slot, "main", is number 1)."""
    value = (_state(user_dir, game) or {}).get('counter')
    return value if isinstance(value, int) and value > 0 else 0


def _bump_counter(user_dir, game, number):
    raw = _state(user_dir, game) or {}
    if number > _counter(user_dir, game):
        raw['counter'] = number
        _write_json(_game_dir(user_dir, game) / 'state.json', raw)


def _next_number(user_dir, game):
    numbers = [int(i[4:]) for i in _slot_ids(user_dir, game) if i.startswith('slot') and i[4:].isdigit()]
    return max([_counter(user_dir, game), 1] + numbers) + 1


def _new_id(user_dir, game):
    """An id never used before for this game's slots (the counter only goes up, so the id of a deleted
    slot stays free of any other slot, and restoring that slot's backup cannot clash)."""
    number = _next_number(user_dir, game)
    _bump_counter(user_dir, game, number)
    return f'slot{number}'


def allocate_id(user_dir, game):
    """A fresh slot id for a slot restored from a backup whose own id is taken."""
    with _lock(user_dir):
        return _new_id(user_dir, game)


def _publish(stage, slot):
    """Moves a finished slot folder into place (a folder left there without slot.json is dropped)."""
    if slot.exists():
        shutil.rmtree(slot)
    os.rename(stage, slot)


def _slot_manifest(game, slot_id, name, now=None):
    return {'id': slot_id, 'game': game, 'name': name, 'created': _stamp(now), 'last_played': '',
            'saved': None, 'title': '', 'subtitle': '', 'detail': ''}


def _state(user_dir, game):
    return _read_json(_game_dir(user_dir, game) / 'state.json')


def _write_state(user_dir, game, state):
    counter = max(int(state.get('counter') or 0), _counter(user_dir, game))
    _write_json(_game_dir(user_dir, game) / 'state.json', {'in_place': state['in_place'],
                                                           'pending': state.get('pending'),
                                                           'counter': counter})


def _ensure(user_dir, game, now=None):
    """The game's slot state; the first time, the save in place becomes the slot "Main" (a copy).
    A state whose slot vanished, or slots without a state, get a new slot for the save in place, so
    it is never written into a slot it may not belong to."""
    state = _state(user_dir, game)
    if state and ID_RE.match(str(state.get('in_place') or '')) and             (_slot_dir(user_dir, game, state['in_place']) / 'slot.json').is_file():
        pending = state.get('pending')
        if pending and not (ID_RE.match(str(pending)) and (_slot_dir(user_dir, game, pending) / 'slot.json').is_file()):
            pending = None  # the slot being swapped in is gone: the save in place stays with in_place
        return {'in_place': state['in_place'], 'pending': pending, 'counter': _counter(user_dir, game)}
    ids = _slot_ids(user_dir, game)
    # "main" is only free while it is the one slot ever made (a counter above 1: it was deleted)
    slot_id = DEFAULT_ID if DEFAULT_ID not in ids and _counter(user_dir, game) <= 1 else _new_id(user_dir, game)
    names = {(_manifest(user_dir, game, i) or {}).get('name', '').casefold() for i in ids}
    name = DEFAULT_NAME
    number = 2
    while name.casefold() in names:
        name = f'{DEFAULT_NAME} {number}'
        number += 1
    # built in a hidden folder and renamed in last: a crash must not leave an empty slot behind
    slot = _slot_dir(user_dir, game, slot_id)
    stage = _game_dir(user_dir, game) / f'.new-{slot_id}'
    shutil.rmtree(stage, ignore_errors=True)
    current = game_units(user_dir, game)
    try:
        (stage / 'data').mkdir(parents=True)
        _copy_units(user_dir, current, stage / 'data')
        manifest = _slot_manifest(game, slot_id, name, now)
        manifest.update(_texts(user_dir, current), saved=_newest(user_dir, current))
        _write_json(stage / 'slot.json', manifest)
        # the state first: a crash before the rename only repeats this setup (slot_id is free again)
        state = {'in_place': slot_id, 'pending': None, 'counter': max(_counter(user_dir, game), 1)}
        _write_state(user_dir, game, state)
        _publish(stage, slot)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return state


def _store(user_dir, game, state):
    """Copies the save in place into the slot it belongs to; True when the slot changed."""
    if state.get('pending'):
        return False  # unclear which slot the save in place is: _finish decides
    slot = _slot_dir(user_dir, game, state['in_place'])
    _fix_data(slot)
    current = game_units(user_dir, game)
    manifest = _read_json(slot / 'slot.json', {})
    texts = _texts(user_dir, current)
    if _digest(user_dir, current) == _digest(slot / 'data', _data_units(slot / 'data')):
        if any(manifest.get(k) != v for k, v in texts.items()):
            manifest.update(texts)
            _write_json(slot / 'slot.json', manifest)
        return False
    stage = slot / '.data-new'
    shutil.rmtree(stage, ignore_errors=True)
    _copy_units(user_dir, current, stage)
    stage.mkdir(exist_ok=True)
    os.rename(slot / 'data', slot / '.data-old')
    os.rename(stage, slot / 'data')
    shutil.rmtree(slot / '.data-old', ignore_errors=True)
    manifest.update(texts, saved=_newest(user_dir, current))
    _write_json(slot / 'slot.json', manifest)
    return True


def _check_running(running):
    if running and running():
        raise SlotError('close the game first', 'running')


def _put_in_place(user_dir, game, state, target, rename):
    """Swaps the slot `target`'s save into the saves folder (the save in place must be stored)."""
    user_dir = Path(user_dir)
    data = _slot_dir(user_dir, game, target) / 'data'
    incoming = _data_units(data)
    current = game_units(user_dir, game)
    stage = Path(tempfile.mkdtemp(prefix='.slot-in-', dir=user_dir))
    aside = Path(tempfile.mkdtemp(prefix='.slot-old-', dir=user_dir))
    try:
        _copy_units(data, incoming, stage)
        _write_state(user_dir, game, {'in_place': state['in_place'], 'pending': target})
        try:
            saves.swap_units(user_dir, sorted(set(current) | set(incoming)), incoming, stage, aside, rename)
        except saves.SaveError as error:
            if getattr(error, 'kept', False):
                aside = None  # the next call sorts it out from the slots' copies (state stays pending)
            else:
                _write_state(user_dir, game, {'in_place': state['in_place'], 'pending': None})
            raise SlotError(str(error))
        state.update(in_place=target, pending=None)
        _write_state(user_dir, game, state)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
        if aside:
            shutil.rmtree(aside, ignore_errors=True)


def _finish(user_dir, game, state, running, rename):
    """A swap was cut short: the save in place is the old slot's (undo), the new one's (done) or a mix
    (backed up, then the new slot's copy is put in place)."""
    target, old = state['pending'], state['in_place']
    current = game_units(user_dir, game)
    here = _digest(user_dir, current)
    for slot_id in (target, old):
        data = _slot_dir(user_dir, game, slot_id) / 'data'
        _fix_data(data.parent)
        if here == _digest(data, _data_units(data)):
            state.update(in_place=slot_id, pending=None)
            _write_state(user_dir, game, state)
            return
    _check_running(running)
    if current:
        saves.create_backup(user_dir, game, kind='before-restore')
    _put_in_place(user_dir, game, {'in_place': old, 'pending': None}, target, rename)
    state.update(in_place=target, pending=None)


# ---- what the launcher calls -----------------------------------------------------------------

def _untouched(user_dir, game):
    """No slots and no save yet: nothing to store or swap (and no slot folder is made for it)."""
    return not _game_dir(user_dir, game).is_dir() and not game_units(user_dir, game)


def in_place(user_dir, game, now=None):
    """The id of the slot whose save is in the saves folder."""
    with _lock(user_dir):
        state = _ensure(user_dir, game, now)
    return state['pending'] or state['in_place']


def list_slots(user_dir, game, now=None):
    """The game's slots, oldest first: id, name, created, last_played, saved (time of the save),
    title/subtitle/detail (the game's texts, subtitle "Campaign 16% Complete - London Underground"),
    empty (no save: the game starts a new campaign) and in_place. The slot in place shows the save
    in the saves folder."""
    with _lock(user_dir):
        state = _ensure(user_dir, game, now)
        current = game_units(user_dir, game)
        found = []
        for slot_id in _slot_ids(user_dir, game):
            manifest = _manifest(user_dir, game, slot_id) or {}
            slot = {'id': slot_id, 'name': str(manifest.get('name') or slot_id),
                    'created': manifest.get('created', ''), 'last_played': manifest.get('last_played', ''),
                    'saved': manifest.get('saved'), 'title': manifest.get('title', ''),
                    'subtitle': manifest.get('subtitle', ''), 'detail': manifest.get('detail', ''),
                    'in_place': slot_id == state['in_place'] and not state['pending']}
            if slot['in_place']:
                slot.update(_texts(user_dir, current), saved=_newest(user_dir, current), empty=not current)
            else:
                slot['empty'] = not _data_units(_slot_dir(user_dir, game, slot_id) / 'data')
            found.append(slot)
    found.sort(key=lambda s: (s['id'] != DEFAULT_ID, s['created'], s['id']))
    return found


def store_back(user_dir, game, now=None):
    """After the game exits (or at the next start): the save in place into its slot. Only copies, so
    it is safe while a game runs; True when the slot changed."""
    with _lock(user_dir):
        return _store(user_dir, game, _ensure(user_dir, game, now))


def store_back_all(user_dir, now=None, stored=None):
    """store_back for every game; returns {game: error message} for those that failed. `stored`, a
    dict, receives {game: slot id} of the slots that changed."""
    errors = {}
    for game in GAMES:
        if _untouched(user_dir, game):
            continue
        try:
            if store_back(user_dir, game, now) and stored is not None:
                stored[game] = in_place(user_dir, game, now)
        except (OSError, saves.SaveError) as error:
            errors[game] = str(error)
    return errors


def recover(user_dir, running=None, rename=os.rename, now=None):
    """At launcher start: finishes a swap cut short (not while a game runs) and stores every save in
    place back into its slot. Returns {game: error message}."""
    errors = {}
    for game in GAMES:
        if _untouched(user_dir, game):
            continue
        try:
            with _lock(user_dir):
                state = _ensure(user_dir, game, now)
                if state['pending']:
                    if running and running():
                        continue
                    _finish(user_dir, game, state, running, rename)
                _store(user_dir, game, state)
        except (OSError, saves.SaveError) as error:
            errors[game] = str(error)
    return errors


def activate(user_dir, game, slot_id, running=None, rename=os.rename, now=None):
    """Puts the slot's save in place (storing the current one into its own slot first). Refused while
    a game runs when a swap is needed. True when the save in place changed."""
    with _lock(user_dir):
        state = _ensure(user_dir, game, now)
        if not (_slot_dir(user_dir, game, slot_id) / 'slot.json').is_file():
            raise SlotError('this save slot no longer exists', 'missing')
        if state['pending']:
            _finish(user_dir, game, state, running, rename)
        _store(user_dir, game, state)
        if state['in_place'] == slot_id:
            return False
        _check_running(running)
        _fix_data(_slot_dir(user_dir, game, slot_id))
        _put_in_place(user_dir, game, state, slot_id, rename)
        return True


def sync(user_dir, active, running=None, rename=os.rename, now=None):
    """Before a game starts: for every game, its active slot (`active`: {game: slot id}; unknown or
    missing ids keep the slot in place) in the saves folder. Returns {game: slot id in place}."""
    placed, swapped = {}, set()
    for game in GAMES:
        wanted = (active or {}).get(game)
        if _untouched(user_dir, game):
            continue
        current = in_place(user_dir, game, now)
        if not wanted or not ID_RE.match(str(wanted)) or \
                not (_slot_dir(user_dir, game, wanted) / 'slot.json').is_file():
            wanted = current
        if activate(user_dir, game, wanted, running, rename, now):
            swapped.add(game)
        placed[game] = wanted
    return Placed(placed, swapped)


class Placed(dict):
    """sync's result: {game: slot id in place}, with .swapped, the games whose save was swapped."""

    def __init__(self, placed, swapped):
        super().__init__(placed)
        self.swapped = swapped


def mark_played(user_dir, game, slot_id, now=None):
    with _lock(user_dir):
        slot = _slot_dir(user_dir, game, slot_id)
        manifest = _read_json(slot / 'slot.json')
        if manifest is not None:
            manifest['last_played'] = _stamp(now)
            _write_json(slot / 'slot.json', manifest)


def _taken(user_dir, game, name, skip=None, shown=None):
    """Whether a slot (other than `skip`) already goes by this name: its stored name, or the name the
    launcher shows for it (`shown`: stored name -> shown name, e.g. the translated "Main")."""
    wanted = name.casefold()
    for slot_id in _slot_ids(user_dir, game):
        if slot_id == skip:
            continue
        stored = str((_manifest(user_dir, game, slot_id) or {}).get('name', ''))
        if wanted in (stored.casefold(), str(shown(stored) if shown else stored).casefold()):
            return True
    return False


def _clean_name(user_dir, game, name, skip=None, shown=None):
    name = ' '.join(str(name or '').split())[:MAX_NAME]
    if not name or any(ord(c) < 32 for c in name):
        raise SlotError('choose a name', 'name-empty')
    if _taken(user_dir, game, name, skip, shown):
        raise SlotError('a slot with this name already exists', 'name-taken')
    return name


def create_slot(user_dir, game, name, copy_from=None, now=None, shown=None):
    """A new slot: empty (the game starts a new campaign) or a copy of the slot `copy_from` (the save
    in place when that slot is in place). Returns its id. `shown`: see _taken."""
    with _lock(user_dir):
        state = _ensure(user_dir, game, now)
        name = _clean_name(user_dir, game, name, shown=shown)
        source = None
        if copy_from:
            if not (_slot_dir(user_dir, game, copy_from) / 'slot.json').is_file():
                raise SlotError('this save slot no longer exists', 'missing')
            if copy_from == state['in_place']:
                _store(user_dir, game, state)
            source = _slot_dir(user_dir, game, copy_from)
            _fix_data(source)
        slot_id = _new_id(user_dir, game)
        stage = _game_dir(user_dir, game) / f'.new-{slot_id}'  # renamed in when complete
        shutil.rmtree(stage, ignore_errors=True)
        try:
            manifest = _slot_manifest(game, slot_id, name, now)
            if source:
                shutil.copytree(source / 'data', stage / 'data')
                original = _read_json(source / 'slot.json', {})
                manifest.update({k: original.get(k, '') for k in ('title', 'subtitle', 'detail')},
                                saved=original.get('saved'))
            else:
                (stage / 'data').mkdir(parents=True)
            _write_json(stage / 'slot.json', manifest)
            _publish(stage, _slot_dir(user_dir, game, slot_id))
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        return slot_id


def duplicate_slot(user_dir, game, slot_id, name, now=None, shown=None):
    return create_slot(user_dir, game, name, copy_from=slot_id, now=now, shown=shown)


def rename_slot(user_dir, game, slot_id, name, shown=None):
    with _lock(user_dir):
        slot = _slot_dir(user_dir, game, slot_id)
        manifest = _read_json(slot / 'slot.json')
        if manifest is None:
            raise SlotError('this save slot no longer exists', 'missing')
        manifest['name'] = _clean_name(user_dir, game, name, skip=slot_id, shown=shown)
        _write_json(slot / 'slot.json', manifest)
        return manifest['name']


def restore_slot(user_dir, game, slot_id, staged, rename=os.rename):
    """Puts a slot out of a restored backup (`staged`: its folder, extracted next to the saves) back.
    Its id is free: it comes back as it was. The id is taken by a slot with other content: it comes
    back under a fresh id, named "<name> (restored)". Same content: nothing to do. Returns
    ('restored', slot id, name), ('renamed', new id, new name) or ('unchanged', slot id, name)."""
    staged = Path(staged)
    name = str((_read_json(staged / 'slot.json', {}) or {}).get('name') or slot_id)
    live = _slot_dir(user_dir, game, slot_id)
    with _lock(user_dir):
        if not (live / 'slot.json').is_file():
            shutil.rmtree(live, ignore_errors=True)  # nothing but a leftover can be there
            live.parent.mkdir(parents=True, exist_ok=True)
            rename(staged, live)
            _bump_counter(user_dir, game, int(slot_id[4:]) if slot_id.startswith('slot') and slot_id[4:].isdigit() else 1)
            return 'restored', slot_id, name
        if _digest(live / 'data', _data_units(live / 'data')) == _digest(staged / 'data', _data_units(staged / 'data')):
            return 'unchanged', slot_id, name
        new_id = _new_id(user_dir, game)
        base, suffix, number = name[:MAX_NAME - 14].rstrip(), ' (restored)', 1
        while _taken(user_dir, game, base + suffix):
            number += 1
            suffix = f' (restored {number})'
        manifest = _read_json(staged / 'slot.json', {}) or {}
        manifest.update(id=new_id, name=base + suffix)
        _write_json(staged / 'slot.json', manifest)
        rename(staged, _slot_dir(user_dir, game, new_id))
        return 'renamed', new_id, base + suffix


def delete_slot(user_dir, game, slot_id, active=None, now=None, backup=True):
    """Deletes a slot; never the one in place, being swapped in, or `active`. First the slot goes into
    a ZIP backup (kind 'before-delete'; restoring it brings the slot back); returns that backup."""
    with _lock(user_dir):
        state = _ensure(user_dir, game, now)
        if slot_id in (state['in_place'], state['pending'], active):
            raise SlotError('the active slot cannot be deleted', 'active')
        slot = _slot_dir(user_dir, game, slot_id)
        if not (slot / 'slot.json').is_file():
            raise SlotError('this save slot no longer exists', 'missing')
        made = saves.create_backup(user_dir, game, kind='before-delete', now=now,
                                   only=[f'save_slots/{game}/{slot_id}']) if backup else None
        shutil.rmtree(slot)
        return made
