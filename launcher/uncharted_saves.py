# SPDX-License-Identifier: GPL-2.0-or-later
"""Save backups for the launcher's Saves page: list, back up, validate, restore.

The port keeps PS4 save data as host folders (src/runtime_savedata.c):
    <user>/savedata/<user id>/<title id>/<dir name>/          the game's files (USR-DATA, ...)
    <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/  param.bin, icon0.png
    <user>/savedata/<user id>/<title id>.memory/              SaveDataMemory
The collection has one title id for all three games; the games name their folders SAVEDATA0 + game
digit + slot (0 = the autosave, P = the profile), SAVEDATA0CC is the collection's shared profile.
Trophy unlocks are <user>/trophy/<set id>.log (lines "<trophy> <time>", the earliest time counts).

A backup is a ZIP with backup_info.json (files with sizes and SHA-256). A restore checks the ZIP, backs
up the current saves, extracts next to the saves, then renames the current folders aside and the new
ones in; any failure puts everything back. Trophy logs are merged, so no unlock is lost.
Design after Canneli's launcher backup (a9dfae6): ZIP with a manifest, checked paths, a backup first.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zipfile

FORMAT = 'uncharted-save-backup-v2'
OLD_FORMAT = 'uncharted-save-backup-v1'   # Canneli's launcher: savedata/ and trophy/ of the user folder
MANIFEST = 'backup_info.json'
GAME_KEYS = ('u1', 'u2', 'u3', 'shared')
SCOPES = GAME_KEYS + ('all',)
PREFIXES = {'u1': 'SAVEDATA01', 'u2': 'SAVEDATA02', 'u3': 'SAVEDATA03', 'shared': 'SAVEDATA0CC'}
KINDS = ('manual', 'auto', 'before-restore', 'before-edit', 'imported')
TITLE_RE = re.compile(r'^[A-Z]{4}\d{5}$')
NAME_RE = re.compile(r'^[A-Za-z0-9_][A-Za-z0-9_.\-]{0,63}$')
MAX_TOTAL = 1 << 30          # uncompressed bytes a backup may hold (saves are a few MB)
MAX_FILES = 20000
STAMP = '%Y%m%d-%H%M%S'


class SaveError(Exception):
    """A backup or restore that cannot be done; the message is for the player."""


# ---- what is on disk -------------------------------------------------------------------------

def save_root(user_dir):
    return Path(user_dir) / 'savedata'


def backup_dir(user_dir):
    return Path(user_dir) / 'backups'


def unit_game(name):
    """'u1'/'u2'/'u3'/'shared' for a save folder name (with or without .sce_sys), else None."""
    base = name[:-len('.sce_sys')] if name.endswith('.sce_sys') else name
    if base.startswith(PREFIXES['shared']):
        return 'shared'
    for key in ('u1', 'u2', 'u3'):
        if base.startswith(PREFIXES[key]):
            return key
    return None


def _title_dirs(user_dir):
    root = save_root(user_dir)
    if not root.is_dir():
        return
    for user in sorted(root.iterdir()):
        if not user.is_dir() or user.name.startswith('.'):
            continue
        for title in sorted(user.iterdir()):
            if title.is_dir() and not title.name.startswith('.') and not title.name.endswith('.memory'):
                yield user, title


def units(user_dir, scope, titles=None):
    """Relative POSIX paths (from the user folder) of what a backup of `scope` holds: save folders
    (each with its .sce_sys), for 'all' also .memory folders and trophy logs. `titles` limits the
    save folders to these 'user/title' pairs."""
    if scope not in SCOPES:
        raise SaveError(f'unknown game {scope!r}')
    found = []
    for user, title in _title_dirs(user_dir):
        pair = f'{user.name}/{title.name}'
        if titles is not None and pair not in titles:
            continue
        for entry in sorted(title.iterdir()):
            if entry.name.startswith('.') or entry.name.endswith('.bbtmp'):
                continue
            if scope == 'all' or unit_game(entry.name) == scope:
                found.append(f'savedata/{pair}/{entry.name}')
        memory = user / f'{title.name}.memory'
        if scope == 'all' and memory.is_dir():
            found.append(f'savedata/{user.name}/{memory.name}')
    if scope == 'all' and titles is None:
        trophy = Path(user_dir) / 'trophy'
        if trophy.is_dir():
            found += [f'trophy/{p.name}' for p in sorted(trophy.glob('*.log')) if p.is_file()]
    return found


def read_param(meta_dir):
    """(title, subtitle, detail, mtime) from a .sce_sys/param.bin, or None."""
    try:
        data = (Path(meta_dir) / 'param.bin').read_bytes()
    except OSError:
        return None
    if len(data) < 1296:
        return None

    def text(start, end):
        return data[start:end].split(b'\0', 1)[0].decode('utf-8', errors='replace').strip()
    mtime = struct.unpack_from('<q', data, 1288)[0]
    return text(0, 128), text(128, 256), text(256, 1280), mtime


def _tree_stats(path):
    """(newest modification time, total size) of a file or folder tree."""
    path = Path(path)
    if path.is_file():
        st = path.stat()
        return st.st_mtime, st.st_size
    newest, total = 0.0, 0
    for p in path.rglob('*'):
        if p.is_file():
            st = p.stat()
            newest, total = max(newest, st.st_mtime), total + st.st_size
    return newest, total


def list_saves(user_dir, describe=None):
    """One dict per save folder: game, slot, title id, path, modified, size, the game's own texts
    (param.bin: subtitle "Campaign 2% Complete - The Search for El Dorado") and, with `describe`
    (save_editor.describe), its result under 'details'."""
    saves = []
    for user, title in _title_dirs(user_dir):
        for entry in sorted(title.iterdir()):
            if not entry.is_dir() or entry.name.endswith('.sce_sys') or entry.name.startswith('.'):
                continue
            game = unit_game(entry.name)
            modified, size = _tree_stats(entry)
            param = read_param(title / f'{entry.name}.sce_sys')
            save = {'game': game, 'dir': entry.name, 'slot': entry.name[len('SAVEDATA0') + 1:] if game else '',
                    'user': user.name, 'title_id': title.name, 'path': entry, 'modified': modified, 'size': size,
                    'name': param[0] if param else '', 'subtitle': param[1] if param else '',
                    'detail': param[2] if param else '', 'details': None}
            if describe:
                try:
                    save['details'] = describe(str(entry))
                except Exception:  # a save the editor cannot read must not break the list
                    save['details'] = None
            saves.append(save)
    return saves


def title_ids(user_dir):
    return sorted({title.name for _user, title in _title_dirs(user_dir)})


# ---- backing up ------------------------------------------------------------------------------

def _files_of(user_dir, unit_list):
    user_dir = Path(user_dir)
    for unit in unit_list:
        path = user_dir / unit
        if path.is_file():
            yield unit, path
        else:
            for p in sorted(path.rglob('*')):
                if p.is_file() and not p.is_symlink():
                    yield p.relative_to(user_dir).as_posix(), p


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def _unique(folder, stem):
    path = folder / f'{stem}.zip'
    number = 2
    while path.exists():
        path = folder / f'{stem}-{number}.zip'
        number += 1
    return path


def create_backup(user_dir, scope, dest=None, kind='manual', game=None, now=None):
    """Writes <dest>/<scope>-<YYYYMMDD-HHMMSS>[-kind].zip (dest: <user>/backups) and returns its path,
    or None when there is nothing to back up. `game`: {'id', 'version', 'title'} of the game folder."""
    if kind not in KINDS:
        raise SaveError(f'unknown backup kind {kind!r}')
    found = units(user_dir, scope)
    if not found:
        return None
    dest = Path(dest) if dest else backup_dir(user_dir)
    dest.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime(STAMP, time.localtime(now))
    files = []
    for rel, path in _files_of(user_dir, found):
        files.append({'path': rel, 'size': path.stat().st_size, 'sha256': _sha256(path)})
    titles = sorted({u.split('/')[2] for u in found if u.startswith('savedata/') and len(u.split('/')) > 3})
    manifest = {'format': FORMAT, 'created': time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(now)),
                'game': scope, 'kind': kind, 'title_ids': titles,
                'game_id': (game or {}).get('id', ''), 'game_version': (game or {}).get('version', ''),
                'game_title': (game or {}).get('title', ''), 'units': found, 'files': files}
    target = _unique(dest, f'{scope}-{stamp}' + ('' if kind == 'manual' else f'-{kind}'))
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=dest, prefix='.backup-', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            archive.writestr(MANIFEST, json.dumps(manifest, indent=2))
            for item in files:
                archive.write(Path(user_dir) / item['path'], item['path'])
        os.replace(temporary, target)
        temporary = None
    finally:
        if temporary:
            try:
                temporary.unlink()
            except OSError:
                pass
    return target


def _content_key(manifest):
    return sorted((f['path'], f['sha256']) for f in manifest.get('files', []))


def auto_backup(user_dir, keep=10, game=None, now=None):
    """The backup taken before a game starts: all saves and trophies, skipped when nothing changed
    since the newest automatic backup; then only the newest `keep` automatic backups stay."""
    found = units(user_dir, 'all')
    if not found:
        return None
    dest = backup_dir(user_dir)
    newest = next((b for b in list_backups(dest) if b['kind'] == 'auto' and b['manifest']), None)
    if newest:
        current = sorted((rel, _sha256(path)) for rel, path in _files_of(user_dir, found))
        if current == _content_key(newest['manifest']):
            return None
    path = create_backup(user_dir, 'all', dest, kind='auto', game=game, now=now)
    prune(dest, keep, kind='auto')
    return path


def prune(dest, keep, kind='auto'):
    """Deletes all but the newest `keep` backups of `kind`; returns the deleted paths."""
    old = [b for b in list_backups(dest) if b['kind'] == kind][max(int(keep), 1):]
    for backup in old:
        try:
            backup['path'].unlink()
        except OSError:
            pass
    return [b['path'] for b in old]


def read_manifest(zip_path):
    try:
        with zipfile.ZipFile(zip_path) as archive:
            return json.loads(archive.read(MANIFEST).decode('utf-8'))
    except (OSError, KeyError, ValueError, zipfile.BadZipFile):
        return None


def list_backups(dest):
    """Backups in a folder, newest first: path, name, game, kind, created, size, manifest (None when
    the ZIP is not a readable backup)."""
    dest = Path(dest)
    found = []
    if not dest.is_dir():
        return found
    for path in dest.glob('*.zip'):
        manifest = read_manifest(path)
        st = path.stat()
        found.append({'path': path, 'name': path.name, 'size': st.st_size, 'manifest': manifest,
                      'game': (manifest or {}).get('game', '?'), 'kind': (manifest or {}).get('kind', 'manual'),
                      'created': (manifest or {}).get('created', ''), 'mtime': st.st_mtime})
    found.sort(key=lambda b: (b['created'], b['mtime'], b['name']), reverse=True)
    return found


# ---- checking a backup -----------------------------------------------------------------------

def _safe_parts(name):
    """The parts of a ZIP member path, or None when it could leave the target folder."""
    if not name or '\\' in name or name.startswith('/') or ':' in name or '\0' in name:
        return None
    parts = PurePosixPath(name).parts
    if not parts or any(p in ('', '.', '..') or p != p.strip() or p.endswith('.') for p in parts):
        return None
    return parts


def _unit_of(parts):
    """The restore unit a member path belongs to: savedata/<user>/<title>/<dir>, savedata/<user>/<t>.memory
    or trophy/<name>.log; None for anything else."""
    if parts[0] == 'trophy' and len(parts) == 2 and parts[1].endswith('.log') and NAME_RE.match(parts[1]):
        return parts
    if parts[0] != 'savedata' or len(parts) < 4 or not parts[1].isdigit():
        return None
    if parts[2].endswith('.memory') and TITLE_RE.match(parts[2][:-len('.memory')]):
        return parts[:3]
    if TITLE_RE.match(parts[2]) and NAME_RE.match(parts[3]) and len(parts) >= 5:
        return parts[:4]
    return None


def validate_backup(zip_path, expected_titles=None):
    """Checks a backup ZIP and returns its manifest (with 'files' filled in for old backups):
    a known format, every member path inside a save unit (no absolute paths, drive letters, '..',
    links), save folders only of the backup's game, files matching the list (size, SHA-256), a sane
    size, and, with `expected_titles`, title ids among them."""
    try:
        archive = zipfile.ZipFile(zip_path)
    except (OSError, zipfile.BadZipFile) as error:
        raise SaveError(f'not a ZIP file ({error})')
    with archive:
        try:
            manifest = json.loads(archive.read(MANIFEST).decode('utf-8'))
        except KeyError:
            raise SaveError('this ZIP is not a save backup (no backup_info.json)')
        except ValueError:
            raise SaveError('the backup description (backup_info.json) is damaged')
        if not isinstance(manifest, dict) or manifest.get('format') not in (FORMAT, OLD_FORMAT):
            raise SaveError('this ZIP is not an Uncharted save backup')
        old = manifest['format'] == OLD_FORMAT
        scope = 'all' if old else manifest.get('game')
        if scope not in SCOPES:
            raise SaveError('the backup names an unknown game')
        members, total, unit_set = {}, 0, set()
        infos = archive.infolist()
        if len(infos) > MAX_FILES:
            raise SaveError('the backup holds too many files')
        for info in infos:
            if info.filename == MANIFEST:
                continue
            parts = _safe_parts(info.filename.rstrip('/') if info.is_dir() else info.filename)
            if parts is None:
                raise SaveError(f'the backup contains an unsafe path: {info.filename}')
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise SaveError(f'the backup contains a link: {info.filename}')
            if info.is_dir():
                continue
            unit = _unit_of(parts)
            if unit is None:
                if old and parts[0] == 'trophy':
                    continue  # Canneli's backups may hold other trophy files: not restored
                raise SaveError(f'the backup contains a file outside the save folders: {info.filename}')
            if unit[0] == 'savedata' and len(unit) == 4 and scope != 'all' and unit_game(unit[3]) != scope:
                raise SaveError(f'the backup of {scope} contains another game\'s save: {info.filename}')
            if unit[0] != 'savedata' or len(unit) == 3:
                if scope != 'all':
                    raise SaveError(f'the backup of {scope} contains {info.filename}')
            name = '/'.join(parts)
            if name in members:
                raise SaveError(f'the backup lists a file twice: {name}')
            members[name] = info
            unit_set.add('/'.join(unit))
            total += info.file_size
            if total > MAX_TOTAL:
                raise SaveError('the backup is too large to be a save backup')
        if not members:
            raise SaveError('the backup contains no save files')
        if old:
            manifest['files'] = [{'path': n, 'size': i.file_size, 'sha256': None} for n, i in members.items()]
        else:
            listed = manifest.get('files')
            if not isinstance(listed, list) or {f.get('path') for f in listed if isinstance(f, dict)} != set(members):
                raise SaveError('the files in the backup do not match its description')
            for item in listed:
                info = members[item['path']]
                if item.get('size') != info.file_size:
                    raise SaveError(f'wrong size in the backup: {item["path"]}')
                digest = hashlib.sha256()
                with archive.open(info) as stream:
                    for block in iter(lambda: stream.read(1 << 20), b''):
                        digest.update(block)
                if digest.hexdigest() != item.get('sha256'):
                    raise SaveError(f'damaged file in the backup: {item["path"]}')
        titles = sorted({u.split('/')[2] for u in unit_set if u.startswith('savedata/') and len(u.split('/')) == 4})
        if expected_titles and titles and not set(titles) <= set(expected_titles):
            raise SaveError('the backup is for {} but these saves are for {}'.format(
                ', '.join(titles), ', '.join(sorted(expected_titles))))
        manifest['game'] = scope
        manifest['units'] = sorted(unit_set)
        manifest['title_ids'] = titles
        return manifest


# ---- restoring -------------------------------------------------------------------------------

def _merge_trophy_log(current, incoming):
    """The union of two unlock logs, earliest time per trophy, as text."""
    times = {}
    for text in (current, incoming):
        for line in text.splitlines():
            part = line.split()
            if len(part) >= 2 and part[0].isdigit() and part[1].isdigit():
                number, when = int(part[0]), int(part[1])
                if number not in times or when < times[number]:
                    times[number] = when
    return ''.join(f'{n} {t}\n' for n, t in sorted(times.items()))


def _write_atomic(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f'.{path.name}.restore.tmp')
    temporary.write_text(text, encoding='utf-8')
    os.replace(temporary, path)


def swap_units(user_dir, unit_list, incoming, stage, aside, rename=os.rename):
    """Moves each unit of `unit_list` (relative POSIX paths from the user folder) that exists into
    `aside` and, for those in `incoming`, the staged copy from `stage` into its place. Any failure
    moves everything back and raises SaveError; when even that fails, the error has kept=True and
    the old units stay in `aside` (named in the message) for a manual fix."""
    user_dir, incoming, journal = Path(user_dir), set(incoming), []
    try:
        for unit in unit_list:
            live = user_dir.joinpath(*unit.split('/'))
            if live.exists():
                away = Path(aside).joinpath(*unit.split('/'))
                away.parent.mkdir(parents=True, exist_ok=True)
                rename(live, away)
                journal.append(('aside', live, away))
            if unit in incoming:
                new = Path(stage).joinpath(*unit.split('/'))
                live.parent.mkdir(parents=True, exist_ok=True)
                rename(new, live)
                journal.append(('in', live, new))
    except OSError as error:
        failed = []
        for action, live, other in reversed(journal):
            try:
                os.rename(live, other) if action == 'in' else os.rename(other, live)
            except OSError:
                failed.append(str(live))
        if failed:
            problem = SaveError('restoring failed ({}) and these could not be put back: {}; the old saves '
                                'are in {}'.format(error, ', '.join(failed), aside))
            problem.kept = True
            raise problem
        raise SaveError(f'restoring failed, nothing was changed ({error})')


def restore_backup(user_dir, zip_path, keep=10, game_running=None, expected_titles=None, game=None,
                   rename=os.rename, now=None):
    """Restores a backup over the saves of its game; returns (manifest, path of the backup made first).
    Save folders of that game in the backup's title folders that the backup lacks are moved away too,
    so the game finds exactly the backed-up state. Trophy logs are merged."""
    if game_running and game_running():
        raise SaveError('close the game first')
    manifest = validate_backup(zip_path, expected_titles)
    scope = manifest['game']
    user_dir = Path(user_dir)
    saves = [u for u in manifest['units'] if u.startswith('savedata/')]
    trophies = [u for u in manifest['units'] if u.startswith('trophy/')]
    pairs = {'/'.join(u.split('/')[1:3]) for u in saves if len(u.split('/')) == 4}
    current = [u for u in units(user_dir, scope, titles=pairs)]
    if scope == 'all':  # .memory folders of the backup's users
        current += [u for u in saves if len(u.split('/')) == 3 and (user_dir / u).exists() and u not in current]
    before = create_backup(user_dir, scope, kind='before-restore', game=game, now=now) if current else None
    if before:
        prune(before.parent, keep, kind='before-restore')

    stamp = time.strftime(STAMP, time.localtime(now))
    stage = Path(tempfile.mkdtemp(prefix=f'.restore-{stamp}-', dir=user_dir))
    aside = Path(tempfile.mkdtemp(prefix=f'.restore-old-{stamp}-', dir=user_dir))
    try:
        with zipfile.ZipFile(zip_path) as archive:
            for item in manifest['files']:
                target = stage.joinpath(*item['path'].split('/'))
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item['path']) as source, open(target, 'wb') as out:
                    shutil.copyfileobj(source, out)
        try:
            swap_units(user_dir, sorted(set(current) | set(saves)), saves, stage, aside, rename)
        except SaveError as error:
            if getattr(error, 'kept', False):
                aside = None  # keep the old saves where they are for a manual fix
            raise
        with zipfile.ZipFile(zip_path) as archive:
            for unit in trophies:
                live = user_dir.joinpath(*unit.split('/'))
                incoming = archive.read(unit).decode('utf-8', errors='replace')
                try:
                    existing = live.read_text(encoding='utf-8', errors='replace')
                except OSError:
                    existing = ''
                _write_atomic(live, _merge_trophy_log(existing, incoming))
    finally:
        shutil.rmtree(stage, ignore_errors=True)
        if aside:
            shutil.rmtree(aside, ignore_errors=True)
    return manifest, before


def import_backup(source, dest):
    """Copies a checked backup ZIP from elsewhere into the backups folder; returns the new path."""
    validate_backup(source)
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    if Path(source).resolve().parent == dest.resolve():
        return Path(source)
    target = _unique(dest, Path(source).stem)
    shutil.copy2(source, target)
    return target


def delete_backup(path, dest):
    """Deletes one backup ZIP, only inside the backups folder."""
    path, dest = Path(path).resolve(), Path(dest).resolve()
    if path.parent != dest or path.suffix.lower() != '.zip':
        raise SaveError('only backups in the backups folder can be deleted')
    path.unlink()


def game_process_running():
    """True while any port game process (bb-probe.exe) runs on this PC (conservative: any copy)."""
    if os.name != 'nt':
        return False
    try:
        out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe', '/NH', '/FO', 'CSV'],
                             capture_output=True, text=True, timeout=10,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return 'bb-probe.exe' in out.lower()
