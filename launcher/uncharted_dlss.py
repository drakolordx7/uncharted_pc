# SPDX-License-Identifier: GPL-2.0-or-later
"""NVIDIA's DLSS runtime DLLs for the launcher.

The package ships only our MIT bridge (bin/bbport_dlss.dll). NVIDIA's nvngx_dlss.dll (Super
Resolution) and nvngx_dlssg.dll (Frame Generation) are not redistributed by us: on first start,
on a GeForce RTX card, the launcher downloads them from NVIDIA's official GitHub
(github.com/NVIDIA/DLSS) at a pinned release, checks them against the SHA-256 pinned below and
puts them next to bb-probe.exe, or into a folder of the launcher's data when that is not
writable. The game finds them there (BB_DLSS_DIR, read by the port's vk_dlss.cpp).
"""
import hashlib
import os
import sys
import tempfile
import urllib.request
from pathlib import Path

# github.com/NVIDIA/DLSS release v310.9.16; the hashes are those of lib/Windows_x86_64/rel in that checkout.
DLSS_TAG = 'v310.9.16'
DLSS_COMMIT = '9981c6350dc58c0ce21f024fd65e31e851c31689'
DLSS_URL = 'https://raw.githubusercontent.com/NVIDIA/DLSS/{ref}/lib/Windows_x86_64/rel/{name}'
# name: (sha256, size in bytes)
DLSS_FILES = {
    'nvngx_dlss.dll': ('d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e', 58956912),
    'nvngx_dlssg.dll': ('8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7', 7506544),
}
BRIDGE = 'bbport_dlss.dll'
_TIMEOUT = 30
_CHUNK = 1 << 20


def _adapter_names():
    """The display adapters' driver descriptions, from the display class key of the registry (the
    same in every session, remote desktop included; EnumDisplayDevices lists only what the session
    shows). Empty when they cannot be read."""
    import winreg
    names = []
    key = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as root:
        for index in range(64):
            try:
                with winreg.OpenKey(root, f'{index:04d}') as adapter:
                    names.append(str(winreg.QueryValueEx(adapter, 'DriverDesc')[0]).upper())
            except OSError:
                continue
    return names


def nvidia_rtx_present():
    """True when a display adapter named NVIDIA ... RTX is installed, False when adapters are
    listed and none is, None when they cannot be listed (not Windows, or the read fails)."""
    if sys.platform != 'win32':
        return None
    try:
        names = _adapter_names()
    except Exception:
        return None
    if not names:
        return None
    return any('NVIDIA' in name and 'RTX' in name for name in names)


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while True:
            block = stream.read(_CHUNK)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def file_ok(path, name):
    """Whether path is the pinned file `name` (size first, then the hash)."""
    expected, size = DLSS_FILES[name]
    try:
        return Path(path).stat().st_size == size and _sha256(path) == expected
    except OSError:
        return False


def _writable(folder):
    try:
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryFile(dir=folder):
            pass
        return True
    except OSError:
        return False


def runtime_folder(bin_dir, fallback_dir):
    """The folder the game should read NVIDIA's DLLs from (BB_DLSS_DIR): bin_dir when it has
    nvngx_dlss.dll, else fallback_dir when it has, else None."""
    for folder in (Path(bin_dir), Path(fallback_dir)):
        if (folder / 'nvngx_dlss.dll').is_file():
            return folder
    return None


def _download(url, temp, expected, size):
    request = urllib.request.Request(url, headers={'User-Agent': 'uncharted-launcher'})
    digest = hashlib.sha256()
    received = 0
    with urllib.request.urlopen(request, timeout=_TIMEOUT) as response, open(temp, 'wb') as out:
        while True:
            block = response.read(_CHUNK)
            if not block:
                break
            received += len(block)
            if received > size:  # not the pinned file: stop early
                return False
            digest.update(block)
            out.write(block)
    return received == size and digest.hexdigest() == expected


def download_dlss_runtime(bin_dir, fallback_dir, require_gpu=True, log=None):
    """Make sure NVIDIA's DLSS DLLs are in bin_dir (or fallback_dir when bin_dir is not writable).
    Does nothing without the bridge in bin_dir, with BB_DLSS=0, or (require_gpu) when no GeForce
    RTX is found; when the adapters cannot be listed it downloads anyway. Each file is fetched
    to a .part name, checked against the pinned size and SHA-256 and only then renamed; files
    that are already right are left alone. Returns the names downloaded. Never raises: a failure
    is one line to `log` and the next start tries again."""
    saved = []
    try:
        bin_dir, fallback_dir = Path(bin_dir), Path(fallback_dir)
        if not (bin_dir / BRIDGE).is_file() or os.environ.get('BB_DLSS', '') == '0':
            return saved
        if require_gpu and nvidia_rtx_present() is False:
            return saved
        target = bin_dir if _writable(bin_dir) else fallback_dir
        for name, (expected, size) in DLSS_FILES.items():
            if file_ok(target / name, name):
                continue
            target.mkdir(parents=True, exist_ok=True)
            temp = target / (name + '.part')
            try:
                if not _download(DLSS_URL.format(ref=DLSS_COMMIT, name=name), temp, expected, size):
                    raise ValueError('size or SHA-256 differs from the pinned file')
                os.replace(temp, target / name)
                saved.append(name)
            except Exception as error:  # offline, HTTP error, wrong file: leave it for the next start
                if log:
                    log(f'DLSS runtime download failed ({name}: {error}); it is retried on the next start.')
                return saved
            finally:
                try:
                    temp.unlink()
                except OSError:
                    pass
    except Exception as error:
        if log:
            log(f'DLSS runtime download failed ({error}); it is retried on the next start.')
    return saved
