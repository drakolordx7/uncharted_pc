"""Save editor for Uncharted: The Nathan Drake Collection (this PC build's saves).

Saves live in <user dir>/savedata/<user id>/<title id>/ (src/runtime_savedata.c), one folder for the
whole collection (CUSA02320 for the US release):
  SAVEDATA010 / 020 / 030   Uncharted 1 / 2 / 3 game save (progress, checkpoint, task list, treasures)
  SAVEDATA01P / 02P / 03P   per-game profile (stats, speed-run times, completed difficulties, U1 rewards)
  SAVEDATA0CC               shared profile (collection settings)
Each folder holds USR-DATA (the data) and ICN-ID; <dir>.sce_sys/param.bin holds the list text.

USR-DATA layout (all files):
  0x00 u32 0xEA82, 0x04 u32 (unknown, left alone), 0x08 u32 9, last 4 bytes: dsize (LE)
  [0x0C, 0x0C+dsize) Blowfish-ECB, key "(SH[@2>r62%5+QKpy|g6", 32-bit words little endian
  plaintext 0x0C: copy of the save list text (title, subtitle, detail, icon path)
  0x58C CRC32 (zlib) of [0x590, 0x590+blocklen-4); 0x590 u32 blocklen, version, pair count,
        section count; 0x5A0 0x400 bytes of text; 0x9A0 pairs (string id, u32);
        then sections: u16 count, u16 entry size, u32 type, entries
  HMAC-SHA1 of [0x0C, dsize-8) at dsize-8 (20 bytes). The game checks both; every edit recomputes them.
String ids: CRC-32 MSB first, polynomial 0x04C11DB7, initial value 0, no final xor.

What the games read (found in the 1.02 executables with IDA; U1 file offsets in docs below):
  * Task list (section 300, 32-byte entries: task id, status, ..., u32 at +28). The +28 field is the
    difficulty the task was played on, -1 if never; Chapter Select lists every chapter whose start
    task has it >= 0 (U1 1.02 sub_138320, U2 1.02 sub_5AD5A0).
  * Completed difficulties: profile pair 0x992F8EE4, bit per difficulty (0 Explorer .. 5 Brutal).
    Crushing is offered when bit 3 is set (U2, U3; U1 uses the reward below or Bluepoint's default),
    Brutal when bit 4 or 5 is set (all three games).
  * U1 rewards (profile section 312, 12-byte entries: category, item, flags): flags bit 0 = unlocked,
    0x10 = turned on. Unlocked entries the game writes read 0x0F.
  * Treasures: section 308, the ids of the treasures found.

Credits: crypto keys and checksum layout from bucanero's naughtydog-decrypter (type C) and the Apollo
Save Tool patch for CUSA02320 (https://github.com/bucanero/save-decrypters,
https://github.com/bucanero/apollo-patches/blob/main/PS4/CUSA02320.savepatch), itself based on
Red-EyeX32 and aerosoul94's tlou_save_data_decrypter. Mevlock's "Unlock all chapters" launcher toggle
(from BilalReloaded's port) unlocks the same menus without touching saves; this editor changes the
save instead, so the unlocks stay without the toggle.

Library use (the launcher's Saves page):
  describe(path) -> str             one short line per save; path: user dir, title folder, SAVEDATA dir or file
  details(path) -> dict             everything decoded, per save
  open_editor(save_dir, game=, parent=)  modal Tk dialog with the edits that apply to that save
  unlock_chapters(path, dry_run=False, difficulty=None) -> list of changes
  unlock_difficulties(path, dry_run=False) -> list of changes
  unlock_rewards(path, dry_run=False) -> list of changes (Uncharted 1)
Every edit copies the original files to <title folder>.backups/<time>/ first (a sibling of the title
folder, which the game never lists) and writes the new file in one rename. Edit only while the game
is closed: the game rewrites its saves.
CLI: python scripts/save_editor.py describe|unlock-chapters|unlock-difficulties|unlock-rewards|
     unlock-all <path> [--game u1|u2|u3] [--dry-run] [--json]
"""
import argparse
import datetime
import hashlib
import hmac
import json
import os
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

BF_KEY = b"(SH[@2>r62%5+QKpy|g6"
HMAC_KEY = b"xM;6X%/p^L/:}-5QoA+K8:F*M!~sb(WK<E%6sW_un0a[7Gm6,()kHoXY+yI/s;Ba"
MAGIC = 0xEA82
CRC_AT, BLOCK_AT, TEXT_AT, PAIRS_AT = 0x58C, 0x590, 0x5A0, 0x9A0

GAMES = {'1': 'u1', '2': 'u2', '3': 'u3'}
GAME_NAMES = {'u1': "Uncharted: Drake's Fortune", 'u2': 'Uncharted 2: Among Thieves', 'u3': "Uncharted 3: Drake's Deception"}
# Public figures (not stored in the save): treasures including the Strange Relic.
TREASURE_TOTAL = {'u1': 61, 'u2': 101, 'u3': 101}
# The collection's difficulty list (U1 1.02 ApplyDifficultySetting: 0 ultra-easy .. 5 ultra-ultra-hard).
DIFFICULTIES = ['Explorer', 'Easy', 'Normal', 'Hard', 'Crushing', 'Brutal']

SEC_TASKS, SEC_TREASURES, SEC_TROPHIES_U1, SEC_UNLOCKABLES = 300, 308, 309, 312
COMPLETED_PAIR = 0x992F8EE4
BIT_HARD, BIT_CRUSHING = 1 << 3, 1 << 4
UNLOCKED = 0x01


def sid(name):
    """Naughty Dog string id of name (CRC-32 MSB first, init 0, no final xor)."""
    if isinstance(name, str):
        name = name.encode()
    c = 0
    for b in name:
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c


PAIR_DIFFICULTY = sid('game-difficulty')
PAIR_TASK = sid('task-subnode-fullname')
U1_UNLOCKABLE_NAMES = {sid(n): n for n in (
    '*unlockables*', '*unlockable-weapons*', '*unlockable-heroes*', '*unlockable-render-modes*', '*unlockable-hidden*',
    'heroes-menu', 'weapon-menu', 'render-menu', 'pistol-semi-a', 'assault-rifle-a', 'shotgun-b', 'pistol-semi-b',
    'pistol-revolver-b', 'shotgun-c', 'hand-grenade-b', 'pistol-revolver-c', 'pistol-full-b', 'smg-c', 'assault-rifle-b',
    'pistol-revolver-eddie', 'grenade-launcher-a', 'sniper-rifle-a', 'hero-naughtydog', 'hero-divesuit', 'hero-sullivan',
    'hero-stylized', 'hero-elena', 'hero-fat', 'hero-redshirt', 'hero-pirate', 'hero-merc', 'hero-eddy', 'hero-roman',
    'hero-navarro', 'black-and-white', 'sepia', 'next-gen', 'mirror-mode', 'fast-motion', 'mirror-mode-alternating',
    'slow-motion', 'super-slow-motion', 'infinite-ammo', 'one-shot-kill', 'ultra-hard-difficulty',
    *('art-gallery-%d' % i for i in range(1, 10)))}
U1_UNLOCKABLE_NAMES.update({0xE56A32CB: 'gallery', 0x06B235A9: 'cheats'})  # category ids without a known name
U1_MENU_CATEGORY = sid('*unlockables*')
U1_CRUSHING = (sid('*unlockable-hidden*'), sid('ultra-hard-difficulty'))


# ---------------------------------------------------------------- Blowfish (pure Python, or pycryptodome)

def _pi_words(count):
    """The first count 32-bit words of pi's fraction (Blowfish's initial P-array and S-boxes)."""
    bits = count * 32 + 64
    one = 1 << bits

    def atan_inv(x):
        total, term, n, sign, x2 = 0, one // x, 1, 1, x * x
        while term:
            total += sign * (term // n)
            term //= x2
            n += 2
            sign = -sign
        return total
    pi = 16 * atan_inv(5) - 4 * atan_inv(239)
    frac = (pi - 3 * one) >> 64
    return [(frac >> (32 * (count - 1 - i))) & 0xFFFFFFFF for i in range(count)]


class _Blowfish:
    def __init__(self, key):
        words = _pi_words(18 + 1024)
        self.p = words[:18]
        self.s = [words[18 + 256 * i:18 + 256 * (i + 1)] for i in range(4)]
        k = len(key)
        for i in range(18):
            v = 0
            for j in range(4):
                v = (v << 8) | key[(4 * i + j) % k]
            self.p[i] ^= v
        left = right = 0
        for i in range(0, 18, 2):
            left, right = self._encrypt(left, right)
            self.p[i], self.p[i + 1] = left, right
        for box in self.s:
            for i in range(0, 256, 2):
                left, right = self._encrypt(left, right)
                box[i], box[i + 1] = left, right

    def _encrypt(self, left, right):
        p, (s0, s1, s2, s3) = self.p, self.s
        for i in range(16):
            left ^= p[i]
            right ^= ((((s0[left >> 24] + s1[(left >> 16) & 255]) & 0xFFFFFFFF) ^ s2[(left >> 8) & 255]) + s3[left & 255]) & 0xFFFFFFFF
            left, right = right, left
        return right ^ p[17], left ^ p[16]

    def _decrypt(self, left, right):
        p, (s0, s1, s2, s3) = self.p, self.s
        for i in range(17, 1, -1):
            left ^= p[i]
            right ^= ((((s0[left >> 24] + s1[(left >> 16) & 255]) & 0xFFFFFFFF) ^ s2[(left >> 8) & 255]) + s3[left & 255]) & 0xFFFFFFFF
            left, right = right, left
        return right ^ p[0], left ^ p[1]

    def run(self, data, encrypt):
        # Big-endian Blowfish on byte-swapped words == words read little endian.
        words = list(struct.unpack('<%dI' % (len(data) // 4), data))
        f = self._encrypt if encrypt else self._decrypt
        for i in range(0, len(words), 2):
            words[i], words[i + 1] = f(words[i], words[i + 1])
        return struct.pack('<%dI' % len(words), *words)


_cipher_cache = {}


def _swap_words(b):
    return b''.join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def _crypt(data, encrypt, pure=False):
    """ECB over data (a multiple of 8 bytes)."""
    if not pure:
        try:
            from Crypto.Cipher import Blowfish  # pycryptodome, optional (faster)
            c = Blowfish.new(BF_KEY, Blowfish.MODE_ECB)
            return _swap_words((c.encrypt if encrypt else c.decrypt)(_swap_words(bytes(data))))
        except ImportError:
            pass
    if 'bf' not in _cipher_cache:
        _cipher_cache['bf'] = _Blowfish(BF_KEY)
    return _cipher_cache['bf'].run(bytes(data), encrypt)


# ---------------------------------------------------------------- file layer

class SaveError(Exception):
    pass


def _dsize(raw):
    if len(raw) < 0x600 or struct.unpack_from('<I', raw, 0)[0] != MAGIC:
        raise SaveError('not a Nathan Drake Collection save (USR-DATA)')
    dsize = struct.unpack_from('<I', raw, len(raw) - 4)[0]
    if dsize < 0x600 or 12 + dsize > len(raw) or dsize & 7:
        raise SaveError('bad data size field')
    return dsize


def decrypt(raw, limit=None):
    """Plaintext copy of a USR-DATA file. limit: decrypt only that many bytes (rest left encrypted)."""
    d = bytearray(raw)
    dsize = _dsize(raw)
    n = dsize if limit is None else min(dsize, (limit + 7) & ~7)
    d[12:12 + n] = _crypt(d[12:12 + n], False)
    return d


def encrypt(plain):
    d = bytearray(plain)
    dsize = _dsize(d)
    d[12:12 + dsize] = _crypt(d[12:12 + dsize], True)
    return d


def _block(d):
    blen = struct.unpack_from('<I', d, BLOCK_AT)[0]
    if blen < 0x414 or BLOCK_AT + blen > len(d):
        raise SaveError('bad block length')
    return blen


def crc_ok(d):
    blen = _block(d)
    return zlib.crc32(bytes(d[BLOCK_AT:BLOCK_AT + blen - 4])) == struct.unpack_from('<I', d, CRC_AT)[0]


def hmac_ok(d):
    dsize = _dsize(d)
    mac = hmac.new(HMAC_KEY, bytes(d[12:dsize - 8]), hashlib.sha1).digest()
    return mac == bytes(d[dsize - 8:dsize + 12])


def fix_checksums(d):
    """Recompute the CRC32 and HMAC-SHA1 the game checks (on plaintext, in place)."""
    blen = _block(d)
    struct.pack_into('<I', d, CRC_AT, zlib.crc32(bytes(d[BLOCK_AT:BLOCK_AT + blen - 4])))
    dsize = _dsize(d)
    d[dsize - 8:dsize + 12] = hmac.new(HMAC_KEY, bytes(d[12:dsize - 8]), hashlib.sha1).digest()


class Save:
    """One decrypted USR-DATA: pairs and sections, with in-place u32 edits."""

    def __init__(self, path, raw=None, full=True):
        self.path = Path(path)
        self.raw = raw if raw is not None else self.path.read_bytes()
        folder = self.path.parent.name.upper()
        m = re.fullmatch(r'SAVEDATA0([123C])([0PC])', folder)
        self.dir = folder
        self.game = GAMES.get(m.group(1)) if m else None
        self.kind = {'0': 'game', 'P': 'profile', 'C': 'shared'}.get(m.group(2)) if m else None
        if full:
            self.data = decrypt(self.raw)
        else:  # enough for describe(): the CRC-covered block
            head = decrypt(self.raw, BLOCK_AT + 8)
            self.data = decrypt(self.raw, BLOCK_AT + _block(head))
        self.full = full
        if not crc_ok(self.data):
            raise SaveError('%s: CRC mismatch (damaged or not decrypted correctly)' % self.path)
        self._parse()
        self.changes = []

    def _parse(self):
        d = self.data
        self.block, self.version, npairs, nsec = struct.unpack_from('<4I', d, BLOCK_AT)
        end = BLOCK_AT + self.block
        self.pairs = {}
        self.pair_at = {}
        for i in range(npairs):
            k, v = struct.unpack_from('<II', d, PAIRS_AT + 8 * i)
            self.pairs[k] = v
            self.pair_at[k] = PAIRS_AT + 8 * i + 4
        self.sections = {}
        o = PAIRS_AT + 8 * npairs
        for _ in range(nsec):
            if o + 8 > end:
                raise SaveError('section table runs past the block')
            h, t = struct.unpack_from('<II', d, o)
            count, size = h & 0xFFFF, h >> 16
            if o + 8 + count * size > end:
                raise SaveError('section %d runs past the block' % t)
            self.sections.setdefault(t, (o + 8, count, size))
            o += 8 + count * size

    def text(self, offset, size):
        return bytes(self.data[offset:offset + size]).split(b'\0')[0].decode('utf-8', 'replace')

    def entries(self, typ, size=None):
        if typ not in self.sections:
            return []
        o, count, esize = self.sections[typ]
        if size is not None and esize != size:
            return []
        return [(o + i * esize, struct.unpack_from('<%dI' % (esize // 4), self.data, o + i * esize)) for i in range(count)]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def set_u32(self, offset, value, what):
        old = self.u32(offset)
        if old != value:
            struct.pack_into('<I', self.data, offset, value)
            self.changes.append({'file': str(self.path), 'field': what, 'offset': offset, 'old': old, 'new': value})

    def encoded(self):
        if not self.full:
            raise SaveError('opened for reading only')
        d = bytearray(self.data)
        fix_checksums(d)
        return encrypt(d)


# ---------------------------------------------------------------- finding saves

def find_saves(path):
    """USR-DATA files under path (user dir, savedata dir, title folder, SAVEDATA folder or the file)."""
    p = Path(path)
    if p.is_file():
        return [p]
    if (p / 'USR-DATA').is_file():
        return [p / 'USR-DATA']
    found = sorted(p.glob('SAVEDATA0*/USR-DATA')) or sorted(p.glob('*/SAVEDATA0*/USR-DATA')) \
        or sorted(p.glob('*/*/SAVEDATA0*/USR-DATA')) or sorted(p.glob('savedata/*/*/SAVEDATA0*/USR-DATA'))
    return [f for f in found if not f.parent.parent.name.endswith('.backups')]


def _open(path, game=None, full=True):
    saves = []
    for f in find_saves(path):
        try:
            s = Save(f, full=full)
        except (SaveError, OSError, struct.error) as e:
            saves.append(e if isinstance(e, SaveError) else SaveError('%s: %s' % (f, e)))
            continue
        if game and s.game != game:
            continue
        saves.append(s)
    return saves


# ---------------------------------------------------------------- describe

def _describe_one(s):
    out = {'file': str(s.path), 'slot': s.dir, 'game': s.game, 'kind': s.kind, 'version': s.version,
           'list_title': s.text(0x0C, 128), 'list_subtitle': s.text(0x8C, 128)}
    if s.kind == 'game':
        detail = s.text(0x10C, 1024)
        out['list_detail'] = detail
        m = re.search(r'(\d+)% Complete - (.+)', out['list_subtitle'])
        out['campaign_percent'] = int(m.group(1)) if m else None
        out['chapter'] = m.group(2).strip() if m else None
        m = re.search(r'Played for (\d+) minute', detail)
        out['playtime_minutes'] = int(m.group(1)) if m else (0 if 'less than 1 minute' in detail else None)
        diff = s.pairs.get(PAIR_DIFFICULTY)
        out['difficulty_index'] = diff
        out['difficulty'] = DIFFICULTIES[diff] if diff is not None and diff < len(DIFFICULTIES) else None
        out['current_task_id'] = '%08x' % s.pairs[PAIR_TASK] if PAIR_TASK in s.pairs else None
        tasks = s.entries(SEC_TASKS, 32)
        played = sum(1 for _, e in tasks if e[7] != 0xFFFFFFFF)
        out['tasks_total'] = len(tasks)
        out['tasks_played'] = played  # tasks with a difficulty recorded; chapter starts are among them
        out['all_chapters_selectable'] = bool(tasks) and played == len(tasks)
        out['treasures_found'] = len(s.entries(SEC_TREASURES, 4))
        out['treasures_total'] = TREASURE_TOTAL.get(s.game)
    elif s.kind == 'profile':
        bits = s.pairs.get(COMPLETED_PAIR)
        out['completed_difficulties'] = None if bits is None else [n for i, n in enumerate(DIFFICULTIES) if bits >> i & 1]
        if bits is not None:
            out['brutal_unlocked'] = bool(bits & 0x30)
            if s.game != 'u1':
                out['crushing_unlocked'] = bool(bits & BIT_HARD)
        if s.game == 'u1':
            rewards = [e for _, e in s.entries(SEC_UNLOCKABLES, 12) if e[0] != U1_MENU_CATEGORY]
            out['rewards_total'] = len(rewards)
            out['rewards_unlocked'] = sum(1 for e in rewards if e[2] & UNLOCKED)
            out['rewards_locked'] = ['%s/%s' % (U1_UNLOCKABLE_NAMES.get(e[0], '%08x' % e[0]), U1_UNLOCKABLE_NAMES.get(e[1], '%08x' % e[1]))
                                     for e in rewards if not e[2] & UNLOCKED]
            crush = [e for _, e in s.entries(SEC_UNLOCKABLES, 12) if (e[0], e[1]) == U1_CRUSHING]
            out['crushing_unlocked'] = bool(crush and crush[0][2] & UNLOCKED) or None  # None: the game's default may still offer it
            trophies = s.entries(SEC_TROPHIES_U1, 12)
            out['medals_total'] = len(trophies)
            out['medals_awarded'] = sum(1 for _, e in trophies if e[1])
    return out


def details(path, game=None):
    """Everything decodable about the saves under path, per file, keyed by slot folder name."""
    result = {'path': str(path), 'saves': {}, 'errors': []}
    for s in _open(path, game, full=False):
        if isinstance(s, SaveError):
            result['errors'].append(str(s))
            continue
        result['saves'][s.dir if s.dir not in result['saves'] else str(s.path)] = _describe_one(s)
    return result


def _yes_no(flag):
    return 'unlocked' if flag else 'locked'


def summary_lines(info):
    """Short plain lines for one save's details (the launcher list and the edit dialog)."""
    lines = []
    if info['kind'] == 'game':
        if info.get('difficulty'):
            lines.append('Difficulty: %s' % info['difficulty'])
        if info.get('treasures_total'):
            lines.append('Treasures: %d of %d' % (info['treasures_found'], info['treasures_total']))
        lines.append('Chapter Select: %s' % ('all chapters' if info.get('all_chapters_selectable') else 'chapters played so far'))
    elif info['kind'] == 'profile':
        done = info.get('completed_difficulties')
        if done is not None:
            lines.append('Finished on: %s' % (', '.join(done) if done else 'none yet'))
        if info.get('crushing_unlocked') is not None:
            lines.append('Crushing: %s' % _yes_no(info['crushing_unlocked']))
        if 'brutal_unlocked' in info:
            lines.append('Brutal: %s' % _yes_no(info['brutal_unlocked']))
        if 'rewards_total' in info:
            lines.append('Rewards: %d of %d unlocked' % (info['rewards_unlocked'], info['rewards_total']))
    elif info['kind'] == 'shared':
        lines.append('Collection settings')
    return lines


def describe(path, game=None):
    """One line per save under path (one line for a SAVEDATA folder), e.g.
    "Difficulty: Easy · Treasures: 0 of 61 · Chapter Select: chapters played so far"."""
    r = details(path, game)
    out = [' · '.join(summary_lines(info)) for info in r['saves'].values()]
    if not out and r['errors']:
        return 'Cannot read this save'
    return chr(10).join(x for x in out if x)


# ---------------------------------------------------------------- edits

def _commit(saves, dry_run):
    """Back up and write every changed save; returns the list of changes."""
    changed = [s for s in saves if s.changes]
    changes = [c for s in changed for c in s.changes]
    if dry_run or not changed:
        return changes
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    blobs = []
    for s in changed:
        blob = bytes(s.encoded())
        check = Save(s.path, raw=blob)  # decrypts, checks CRC and parses again
        if not hmac_ok(check.data) or len(blob) != len(s.raw):
            raise SaveError('%s: re-encoded save failed its own check, nothing written' % s.path)
        blobs.append((s, blob))
    for s, blob in blobs:
        title = s.path.parent.parent
        backup = title.parent / (title.name + '.backups') / stamp / s.dir / s.path.name
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(s.path, backup)
        for c in s.changes:
            c['backup'] = str(backup)
    for s, blob in blobs:
        tmp = s.path.with_name(s.path.name + '.edtmp')
        with open(tmp, 'wb') as f:
            f.write(blob)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, s.path)
    return changes


def _editable(path, game, kind):
    saves = []
    for s in _open(path, game, full=True):
        if isinstance(s, SaveError):
            raise s
        if s.kind == kind:
            if not hmac_ok(s.data):
                raise SaveError('%s: HMAC mismatch, refusing to edit' % s.path)
            saves.append(s)
    return saves


def unlock_chapters(path, dry_run=False, difficulty=None, game=None):
    """Every chapter selectable in Chapter Select (game saves): each task never played gets a difficulty
    recorded, the save's own difficulty unless one is given. Tasks already played keep theirs."""
    saves = _editable(path, game, 'game')
    if not saves:
        raise SaveError('no game save (SAVEDATA010/020/030) under %s' % path)
    for s in saves:
        value = s.pairs.get(PAIR_DIFFICULTY) if difficulty is None else difficulty
        if value is None or not 0 <= value < len(DIFFICULTIES):
            raise SaveError('%s: no usable difficulty (%r)' % (s.path, value))
        for off, e in s.entries(SEC_TASKS, 32):
            if e[7] == 0xFFFFFFFF:
                s.set_u32(off + 28, value, 'task %08x difficulty' % e[0])
    return _commit(saves, dry_run)


def unlock_difficulties(path, dry_run=False, game=None):
    """Crushing and Brutal offered in the difficulty menus (profiles): marks the game completed on Hard
    and Crushing, and for Uncharted 1 also unlocks the Crushing reward."""
    saves = _editable(path, game, 'profile')
    if not saves:
        raise SaveError('no profile (SAVEDATA01P/02P/03P) under %s' % path)
    for s in saves:
        if COMPLETED_PAIR not in s.pairs:
            raise SaveError('%s: completed-difficulty field not found' % s.path)
        s.set_u32(s.pair_at[COMPLETED_PAIR], s.pairs[COMPLETED_PAIR] | BIT_HARD | BIT_CRUSHING, 'completed difficulties')
        if s.game == 'u1':
            for off, e in s.entries(SEC_UNLOCKABLES, 12):
                if (e[0], e[1]) == U1_CRUSHING:
                    s.set_u32(off + 8, e[2] | UNLOCKED | 0x08, 'reward ultra-hard-difficulty')
    return _commit(saves, dry_run)


def unlock_rewards(path, dry_run=False, game=None):
    """Uncharted 1 rewards (skins, weapons, render modes, art gallery, cheats) unlocked in the profile.
    Uncharted 2 and 3 rewards are bought in the in-game store and are not in a known save field."""
    saves = [s for s in _editable(path, game, 'profile') if s.game == 'u1']
    if not saves:
        raise SaveError('no Uncharted 1 profile (SAVEDATA01P) under %s' % path)
    for s in saves:
        for off, e in s.entries(SEC_UNLOCKABLES, 12):
            if not e[2] & UNLOCKED:
                name = U1_UNLOCKABLE_NAMES.get(e[1], '%08x' % e[1])
                s.set_u32(off + 8, e[2] | UNLOCKED | 0x08, 'reward %s' % name)
    return _commit(saves, dry_run)


# ---------------------------------------------------------------- edit dialog (launcher Saves page)

DIALOG_COLORS = {'bg': '#17181b', 'panel': '#202226', 'text': '#f3efe6', 'muted': '#8e8a82', 'accent': '#e7c36a',
                 'ok': '#9fd09a', 'error': '#e88b7d'}


def actions_for(save_dir):
    """[(label, confirm question, function(path, dry_run))] for the edits that apply to this save."""
    s = Save(find_saves(save_dir)[0], full=False)
    acts = []
    if s.kind == 'game':
        acts.append(('Unlock all chapters', 'Make every chapter selectable in Chapter Select?',
                     lambda p, dry: unlock_chapters(p, dry)))
    elif s.kind == 'profile':
        acts.append(('Unlock Crushing and Brutal', 'Unlock the Crushing and Brutal difficulties?',
                     lambda p, dry: unlock_difficulties(p, dry)))
        if s.game == 'u1':
            acts.append(('Unlock all rewards', 'Unlock all skins, weapons, render modes, art and cheats?',
                         lambda p, dry: unlock_rewards(p, dry)))
    if len(acts) > 1:
        fns = [a[2] for a in acts]
        acts.append(('Unlock everything', 'Unlock everything listed above?',
                     lambda p, dry: [c for f in fns for c in f(p, dry)]))
    return s, acts


def open_editor(save_dir, game=None, parent=None, wait=True):
    """Modal dialog on parent (a Tk window) for one SAVEDATA folder: what the save holds and a button per
    edit, each asking first. Returns when the dialog is closed (wait=False: returns the dialog at once;
    dialog.run(label, confirm=False, dry_run=True) runs an action without asking)."""
    import tkinter as tk
    from tkinter import messagebox
    save, acts = actions_for(save_dir)
    game = save.game or game
    c = dict(DIALOG_COLORS)
    try:
        c['bg'] = parent.cget('bg') if parent is not None and parent.cget('bg').startswith('#') else c['bg']
    except Exception:
        pass
    win = tk.Toplevel(parent)
    kind = {'game': 'game save', 'profile': 'profile', 'shared': 'shared settings'}.get(save.kind, 'save')
    win.title('Edit %s - %s' % (kind, GAME_NAMES.get(game, 'Uncharted')))
    win.configure(bg=c['bg'], padx=18, pady=14)
    win.resizable(False, False)
    if parent is not None:
        win.transient(parent)
    tk.Label(win, text='%s · %s' % (GAME_NAMES.get(game, 'Uncharted'), kind), bg=c['bg'], fg=c['text'],
             font=('Segoe UI', 12, 'bold'), anchor='w').pack(fill='x')
    info = tk.Label(win, bg=c['bg'], fg=c['muted'], font=('Segoe UI', 10), anchor='w', justify='left')
    info.pack(fill='x', pady=(4, 10))
    result = tk.Label(win, text='', bg=c['bg'], fg=c['muted'], font=('Segoe UI', 10), anchor='w', justify='left',
                      wraplength=420)

    def refresh():
        try:
            r = details(save_dir)
            one = next(iter(r['saves'].values()), None)
            lines = []
            if one:
                if one.get('list_subtitle') and one['kind'] == 'game':
                    lines.append(one['list_subtitle'])
                lines += summary_lines(one)
            info.configure(text='\n'.join(lines) or 'Cannot read this save')
        except Exception as e:  # keep the dialog usable
            info.configure(text='Cannot read this save: %s' % e)

    def run(label, confirm=True, dry_run=False):
        for name, question, fn in acts:
            if name != label:
                continue
            if confirm and not messagebox.askyesno(label, question + '\n\nA backup is made first.', parent=win):
                return None
            try:
                changes = fn(save_dir, dry_run)
            except SaveError as e:
                result.configure(text='Could not change the save: %s' % e, fg=c['error'])
                return None
            if dry_run:
                text = 'Would change %d value(s).' % len(changes) if changes else 'Nothing to change.'
            else:
                text = 'Done.' if changes else 'Already unlocked, nothing to change.'
            result.configure(text='%s: %s' % (label, text), fg=c['ok'] if changes else c['muted'])
            refresh()
            return changes
        raise KeyError(label)

    for name, _question, _fn in acts:
        tk.Button(win, text=name, command=lambda n=name: run(n), bg=c['panel'], fg=c['text'],
                  activebackground=c['accent'], activeforeground='#000000', relief='flat', bd=0, padx=14, pady=6,
                  font=('Segoe UI', 10), cursor='hand2').pack(fill='x', pady=3)
    if not acts:
        tk.Label(win, text='Nothing to edit in this save.', bg=c['bg'], fg=c['muted'], font=('Segoe UI', 10),
                 anchor='w').pack(fill='x')
    result.pack(fill='x', pady=(10, 4))
    tk.Button(win, text='Close', command=win.destroy, bg=c['bg'], fg=c['muted'], activebackground=c['panel'],
              activeforeground=c['text'], relief='flat', bd=0, padx=14, pady=4, font=('Segoe UI', 10),
              cursor='hand2').pack(anchor='e', pady=(6, 0))
    win.bind('<Escape>', lambda _e: win.destroy())
    refresh()
    win.run = run
    win.actions = [a[0] for a in acts]
    if wait:
        try:
            win.grab_set()
        except Exception:
            pass
        win.focus_set()
        win.wait_window()
    return win


# ---------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description='Uncharted collection save editor (back up first; edit with the game closed).')
    ap.add_argument('command', choices=['describe', 'unlock-chapters', 'unlock-difficulties', 'unlock-rewards', 'unlock-all'])
    ap.add_argument('path', help='user dir, title folder (e.g. savedata/1/CUSA02320), SAVEDATA folder or USR-DATA file')
    ap.add_argument('--game', choices=['u1', 'u2', 'u3'], help='only this game')
    ap.add_argument('--difficulty', choices=[d.lower() for d in DIFFICULTIES],
                    help='unlock-chapters: difficulty to record (default: the save\'s own)')
    ap.add_argument('--dry-run', action='store_true', help='show what would change, write nothing')
    ap.add_argument('--json', action='store_true', help='print JSON')
    a = ap.parse_args(argv)
    try:
        if a.command == 'describe':
            r = details(a.path, a.game)
            if a.json:
                print(json.dumps(r, indent=2))
            else:
                for slot, info in r['saves'].items():
                    print('%s (%s %s)' % (slot, GAME_NAMES.get(info['game'], 'Collection'), info['kind']))
                    for k, v in info.items():
                        if k not in ('file', 'slot', 'game', 'kind') and v not in (None, '', []):
                            print('  %-24s %s' % (k, str(v).strip().replace(chr(10), ' | ')))
                for e in r['errors']:
                    print('error:', e)
            return 1 if r['errors'] and not r['saves'] else 0
        diff = DIFFICULTIES.index(a.difficulty.capitalize()) if a.difficulty else None
        jobs = {'unlock-chapters': [lambda: unlock_chapters(a.path, a.dry_run, diff, a.game)],
                'unlock-difficulties': [lambda: unlock_difficulties(a.path, a.dry_run, a.game)],
                'unlock-rewards': [lambda: unlock_rewards(a.path, a.dry_run, a.game)]}
        jobs['unlock-all'] = jobs['unlock-chapters'] + jobs['unlock-difficulties'] + \
            ([] if a.game in ('u2', 'u3') else jobs['unlock-rewards'])
        changes = []
        for job in jobs[a.command]:
            changes += job()
        if a.json:
            print(json.dumps({'dry_run': a.dry_run, 'changes': changes}, indent=2))
        else:
            per_file = {}
            for c in changes:
                per_file.setdefault(c['file'], []).append(c)
            for f, cs in per_file.items():
                print('%s: %d field(s)%s' % (f, len(cs), ' would change' if a.dry_run else ' changed'))
                for c in cs[:8]:
                    print('  %-40s %d -> %d' % (c['field'], c['old'], c['new']))
                if len(cs) > 8:
                    print('  ... and %d more' % (len(cs) - 8))
                if not a.dry_run and cs[0].get('backup'):
                    print('  backup:', Path(cs[0]['backup']).parent.parent)
            if not changes:
                print('nothing to change')
        return 0
    except SaveError as e:
        print('error:', e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
