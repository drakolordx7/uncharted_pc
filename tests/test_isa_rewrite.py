"""Site classification and BBISA1 table tests for scripts/isa_rewrite.py, on hand-written machine code."""
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import isa_rewrite as r  # noqa: E402

ADDR = 0x1000


def sites_of(hexcode, address=ADDR):
    return r.scan_segment(bytes.fromhex(hexcode), address)


@unittest.skipUnless(r.iced_x86 is not None, 'iced_x86 is not installed')
class ScanTests(unittest.TestCase):
    def one(self, hexcode):
        found = sites_of(hexcode)
        self.assertEqual(len(found), 1, f'{hexcode}: expected one site, got {len(found)}')
        return found[0]

    def test_blsr_register_form(self):
        s = self.one('c4e270f3c9')  # blsr ecx, ecx
        self.assertEqual(s.kind, r.KIND_BLSR, 'blsr kind')
        self.assertEqual(s.width, 32, 'blsr width')
        self.assertEqual(s.mode, r.MODE_JMP, 'blsr is 5 bytes, so MODE_JMP')
        self.assertEqual(s.length, 5, 'blsr length')
        self.assertEqual((s.offset, s.dst, s.src1), (ADDR, 1, 1), 'blsr dst/src1 are both ecx')
        self.assertEqual(s.src2, r.NO_REG, 'blsr has no third register')

    def test_andn_three_registers(self):
        s = self.one('c4e260f2c1')  # andn eax, ebx, ecx
        self.assertEqual(s.kind, r.KIND_ANDN, 'andn kind')
        self.assertEqual(s.width, 32, 'andn width')
        self.assertEqual(s.mode, r.MODE_JMP, 'andn is 5 bytes, so MODE_JMP')
        self.assertEqual(s.length, 5, 'andn length')
        self.assertEqual((s.dst, s.src1, s.src2), (0, 3, 1),
                         'andn dst=eax(0), src1=ebx(3, VEX.vvvv), src2=ecx(1, rm)')

    def test_bextr_rip_relative_memory_source(self):
        s = self.one('c4e270f7050c000000')  # bextr eax, [rip+0xc], ecx
        self.assertEqual(s.kind, r.KIND_BEXTR, 'bextr kind')
        self.assertEqual(s.width, 32, 'bextr width')
        self.assertEqual(s.mode, r.MODE_JMP, 'bextr is 9 bytes, so MODE_JMP')
        self.assertEqual(s.length, 9, 'bextr length')
        self.assertEqual(s.mem_slot, 2, 'memory is the second operand, so mem_slot 2')
        self.assertTrue(s.mem_flags & 1, 'RIP-relative flag must be set')
        self.assertEqual(s.disp, ADDR + 9 + 0xC, 'RIP-relative disp is the absolute target')
        self.assertEqual((s.dst, s.src2), (0, 1), 'bextr dst=eax, src2=ecx')
        self.assertEqual(s.src1, r.NO_REG, 'memory operand leaves src1 empty')

    def test_lzcnt_16_bit(self):
        s = self.one('66f30fbdc1')  # lzcnt ax, cx
        self.assertEqual(s.kind, r.KIND_LZCNT, 'lzcnt kind')
        self.assertEqual(s.width, 16, 'lzcnt ax width')
        self.assertEqual(s.mode, r.MODE_JMP, 'lzcnt is 5 bytes, so MODE_JMP')

    def test_tzcnt_64_bit(self):
        s = self.one('f3480fbcc1')  # tzcnt rax, rcx
        self.assertEqual(s.kind, r.KIND_TZCNT, 'tzcnt kind')
        self.assertEqual(s.width, 64, 'tzcnt rax width')
        self.assertEqual(s.mode, r.MODE_JMP, 'tzcnt is 5 bytes, so MODE_JMP')

    def test_movbe_store_relocates_following_nop(self):
        s = self.one('0f38f10890')  # movbe [rax], ecx; nop
        self.assertEqual(s.kind, r.KIND_MOVBE_STORE, 'movbe to memory is the store kind')
        self.assertEqual(s.length, 4, 'movbe store length')
        self.assertEqual(s.mode, r.MODE_JMP_RELOCATE, 'plain nop after a 4-byte site is relocated')
        self.assertEqual(s.relocated, b'\x90', 'relocated bytes are the nop')
        self.assertEqual(s.mem_slot, 1, 'memory is the destination, so mem_slot 1')
        self.assertEqual(s.base, 0, 'base register is rax')

    def test_movbe_load_kind(self):
        s = self.one('0f38f008')  # movbe ecx, [rax]
        self.assertEqual(s.kind, r.KIND_MOVBE_LOAD, 'movbe from memory is the load kind')
        self.assertEqual(s.dst, 1, 'load destination is ecx')
        self.assertEqual(s.mem_slot, 2, 'memory is the source, so mem_slot 2')

    def test_movbe_followed_by_jmp_self_is_int3(self):
        found = sites_of('0f38f108ebfe')  # movbe [rax], ecx; jmp $
        self.assertEqual(len(found), 1, 'the jmp is not a site')
        self.assertEqual(found[0].mode, r.MODE_INT3, 'a branch after the site cannot be relocated')
        self.assertEqual(found[0].relocated, b'', 'INT3 sites carry no relocated bytes')

    def test_nop_that_is_a_branch_target_is_int3(self):
        # 0x1000: eb 04 jmp short to 0x1006 (the nop); 0x1002: movbe [rax], ecx; 0x1006: nop; 0x1007: ret
        found = sites_of('eb040f38f10890c3')
        self.assertEqual(len(found), 1, 'expected one movbe site')
        self.assertEqual(found[0].offset, ADDR + 2, 'movbe sits at 0x1002')
        self.assertEqual(found[0].mode, r.MODE_INT3, 'the nop is a jump target, so no relocation')

    def test_jump_table_and_relocation_targets_stay_int3(self):
        # lea rax,[rip+table]; movbe [rax],ecx; nop; ret -- a 32-bit switch table entry points at the nop
        import struct
        code = bytes.fromhex('488d05f9070000' '0f38f108' '90' 'c3')   # table at 0x1800: 0x1800 - (0x1000 + 7)
        image = bytearray(0x2000)
        image[0x1000:0x1000 + len(code)] = code
        struct.pack_into('<i', image, 0x1800, 0x100b - 0x1800)
        plain = r.scan_segment(code, ADDR)
        self.assertEqual(plain[0].mode, r.MODE_JMP_RELOCATE, 'without the table the nop looks free to move')
        tabled = r.scan_segment(code, ADDR, bytes(image))
        self.assertEqual(tabled[0].mode, r.MODE_INT3, 'a switch table entry points at the moved nop')
        pointed = r.scan_segment(code, ADDR, None, {0x100b})
        self.assertEqual(pointed[0].mode, r.MODE_INT3, 'a relocation points at the moved nop')

    def test_movbe_at_end_of_segment_is_int3(self):
        found = sites_of('0f38f108')
        self.assertEqual(len(found), 1, 'expected one movbe site')
        self.assertEqual(found[0].mode, r.MODE_INT3, 'no successor inside the segment')
        self.assertEqual(found[0].length, 4, 'movbe length')

    def test_fs_segment_override_is_unsupported(self):
        s = self.one('64f3480fbd00')  # fs lzcnt rax, [rax]
        self.assertEqual(s.kind, r.KIND_LZCNT, 'lzcnt kind')
        self.assertEqual(s.mode, r.MODE_UNSUPPORTED, 'segment override is not emulated')
        self.assertEqual(s.length, 6, 'fs lzcnt length')

    def test_lock_prefixed_vex_decodes_as_no_site(self):
        # iced-x86 decodes LOCK on a VEX instruction as invalid, so no site is produced.
        # The LOCK branch of site_of cannot be reached through a valid encoding of these mnemonics.
        self.assertEqual(sites_of('f0c4e270f308'), [], 'lock + blsr memory form is invalid, no site')

    def test_vcvtph2ps_xmm_register_form_supported(self):
        s = self.one('c4e27913c0')  # vcvtph2ps xmm0, xmm0
        self.assertEqual(s.kind, r.KIND_VCVTPH2PS, 'vcvtph2ps kind')
        self.assertEqual(s.mode, r.MODE_JMP, 'vcvtph2ps xmm,xmm is 5 bytes, so MODE_JMP')
        self.assertEqual((s.dst, s.src1), (0, 0), 'dst=xmm0, src1=xmm0')
        self.assertEqual(s.width, 0, 'F16C sites carry no width')

    def test_f16c_other_forms_unsupported(self):
        cases = {
            'vcvtph2ps ymm0, xmm0': 'c4e27d13c0',
            'vcvtph2ps xmm0, [rax]': 'c4e2791300',
        }
        for name, hexcode in cases.items():
            with self.subTest(name):
                s = self.one(hexcode)
                self.assertEqual(s.kind, r.KIND_VCVTPH2PS, 'kind')
                self.assertEqual(s.mode, r.MODE_UNSUPPORTED, 'not emulated')

    def test_vcvtps2ph_unsupported(self):
        s = self.one('c4e37d1dc000')  # vcvtps2ph xmm0, ymm0, 0
        self.assertEqual(s.kind, r.KIND_VCVTPS2PH, 'vcvtps2ph kind')
        self.assertEqual(s.mode, r.MODE_UNSUPPORTED, 'vcvtps2ph is never emulated')

    def test_no_sites_in_plain_code(self):
        self.assertEqual(sites_of('90c3'), [], 'nop and ret are not sites')


@unittest.skipUnless(r.iced_x86 is not None, 'iced_x86 is not installed')
class TableTests(unittest.TestCase):
    def test_record_and_header_sizes(self):
        self.assertEqual(r.RECORD.size, 64, 'RECORD size')
        self.assertEqual(r.HEADER.size, 64, 'HEADER size')

    def test_encode_table_round_trip(self):
        # blsr @0x1000, movbe store @0x1005 (relocates the nop @0x1009), fs lzcnt @0x100a (unsupported)
        code = bytes.fromhex('c4e270f3c9' '0f38f10890' '64f3480fbd00' 'c3')
        found = r.scan_segment(code, ADDR)
        self.assertEqual([s.offset for s in found], [ADDR, ADDR + 5, ADDR + 10], 'site offsets')
        image = code + bytes(16)

        # Pass the sites in reverse order: the table must still come out sorted by offset.
        table = r.encode_table(image, list(reversed(found)))
        count = len(found)

        magic, size, digest, sites, unsupported = r.HEADER.unpack_from(table, 0)
        self.assertEqual(magic, b'BBISA1\0\0', 'magic')
        self.assertEqual(size, len(image), 'image size in header')
        self.assertEqual(digest, hashlib.sha256(image).digest(), 'sha256 of the image')
        self.assertEqual(sites, count, 'record count in header')
        self.assertEqual(unsupported, 1, 'unsupported count in header')
        self.assertEqual(len(table), 64 + 64 * count, 'total length is header plus records')

        offsets = [r.RECORD.unpack_from(table, 64 + 64 * i)[0] for i in range(count)]
        self.assertEqual(offsets, sorted(offsets), 'records are in ascending offset order')
        self.assertEqual(offsets, [s.offset for s in found], 'records match the sites')

        for i, s in enumerate(found):
            with self.subTest(offset=hex(s.offset)):
                (offset, length, kind, width, mode, dst, src1, src2, mem_slot, base, index, scale,
                 mem_flags, disp, original, reloc_len, relocated) = r.RECORD.unpack_from(table, 64 + 64 * i)
                self.assertEqual((offset, length, kind, width, mode), (s.offset, s.length, s.kind, s.width, s.mode),
                                 'offset, length, kind, width, mode')
                self.assertEqual((dst, src1, src2, mem_slot), (s.dst, s.src1, s.src2, s.mem_slot),
                                 'register fields and mem_slot')
                self.assertEqual((base, index, scale, mem_flags, disp),
                                 (s.base, s.index, s.scale, s.mem_flags, s.disp), 'memory fields')
                self.assertEqual(original[:length], s.original, 'original instruction bytes')
                self.assertEqual(reloc_len, len(s.relocated), 'relocated length')
                self.assertEqual(relocated[:reloc_len], s.relocated, 'relocated bytes')


if __name__ == '__main__':
    unittest.main()
