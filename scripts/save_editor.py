"""Uncharted: The Nathan Drake Collection save files (WIP: crypto layer only).

USR-DATA layout (all three games, one title id CUSA02320 folder per user):
  0x00 u32 magic 0x0000EA82, 0x04 u32 (unknown, not touched), 0x08 u32 9
  0x0C .. 0x0C+dsize  Blowfish-ECB encrypted, each 32-bit word byte-swapped
  last 4 bytes        dsize (little endian)
Decrypted: 0x0C.. a copy of the save's param (title/subtitle/detail/icon path),
  0x58C u32 CRC32 (zlib) of [0x590, 0x590+blocklen-4), 0x590 u32 blocklen,
  HMAC-SHA1 over [0x0C, dsize-8) stored at dsize-8 (20 bytes).
Keys and layout from bucanero's naughtydog-decrypter (type C) and Apollo
patch CUSA02320.savepatch:
  https://github.com/bucanero/save-decrypters/tree/master/naughtydog-decrypter
  https://github.com/bucanero/apollo-patches/blob/main/PS4/CUSA02320.savepatch
"""
import hashlib
import hmac
import struct
import sys
import zlib

BF_KEY = b"(SH[@2>r62%5+QKpy|g6"
HMAC_KEY = b"xM;6X%/p^L/:}-5QoA+K8:F*M!~sb(WK<E%6sW_un0a[7Gm6,()kHoXY+yI/s;Ba"
MAGIC = 0xEA82


def _swap_words(b):
    n = len(b) & ~3
    return b"".join(b[i:i + 4][::-1] for i in range(0, n, 4)) + b[n:]


def _cipher():
    from Crypto.Cipher import Blowfish  # pycryptodome (pure-Python fallback: TODO)
    return Blowfish.new(BF_KEY, Blowfish.MODE_ECB)


def decrypt(raw):
    d = bytearray(raw)
    if struct.unpack_from("<I", d, 0)[0] != MAGIC:
        raise ValueError("not a Nathan Drake Collection USR-DATA file")
    dsize = struct.unpack_from("<I", d, len(d) - 4)[0]
    n = dsize & ~7
    d[12:12 + n] = _swap_words(_cipher().decrypt(_swap_words(bytes(d[12:12 + n]))))
    return d


def fix_checksums(d):
    dsize = struct.unpack_from("<I", d, len(d) - 4)[0]
    blen = struct.unpack_from("<I", d, 0x590)[0]
    struct.pack_into("<I", d, 0x58C, zlib.crc32(bytes(d[0x590:0x590 + blen - 4])))
    d[dsize - 8:dsize + 12] = hmac.new(HMAC_KEY, bytes(d[12:12 + dsize - 0x14]), hashlib.sha1).digest()


def check(d):
    dsize = struct.unpack_from("<I", d, len(d) - 4)[0]
    blen = struct.unpack_from("<I", d, 0x590)[0]
    crc = zlib.crc32(bytes(d[0x590:0x590 + blen - 4])) == struct.unpack_from("<I", d, 0x58C)[0]
    mac = hmac.new(HMAC_KEY, bytes(d[12:12 + dsize - 0x14]), hashlib.sha1).digest() == bytes(d[dsize - 8:dsize + 12])
    return crc, mac


def encrypt(d):
    d = bytearray(d)
    dsize = struct.unpack_from("<I", d, len(d) - 4)[0]
    n = dsize & ~7
    d[12:12 + n] = _swap_words(_cipher().encrypt(_swap_words(bytes(d[12:12 + n]))))
    return d


if __name__ == "__main__":
    for p in sys.argv[1:]:
        raw = open(p, "rb").read()
        d = decrypt(raw)
        print(p, "crc/hmac ok:", check(d), "round trip identical:", bytes(encrypt(d)) == raw)
