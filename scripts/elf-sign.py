#!/usr/bin/env python3
# ELF Signing Tool for glibc dl-secure
# Copyright (C) 2024 Free Software Foundation, Inc.
#
# This file is part of the GNU C Library.
#
# The GNU C Library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# The GNU C Library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with the GNU C Library; if not, see
# <https://www.gnu.org/licenses/>.

"""Sign, verify, or hash ELF binaries for use with ld.so's secure loader.

This tool computes HMAC-SHA256 signatures over PT_LOAD segments
and embeds them in a .note.dl-secure ELF section.

Usage:
  python3 elf-sign.py --key <keyfile> --sign <binary>
  python3 elf-sign.py --key <keyfile> --verify <binary>
  python3 elf-sign.py --hash <binary>
"""

import argparse
import hashlib
import hmac
import struct
import sys
import os
import tempfile
import shutil

# Constants matching dl-secure.h
DL_SECURE_NOTE_NAME = b'DL-Secure\x00'
DL_SECURE_NOTE_TYPE = 0x53454300  # "SEC\0"
DL_SECURE_HASH_SIZE = 32

# ELF constants
PT_LOAD = 1
SHT_NOTE = 7
SHT_STRTAB = 3


class ELFFile:
    """Minimal ELF parser using only the struct module."""

    def __init__(self, data):
        self.data = bytearray(data)

        # Check ELF magic
        if self.data[:4] != b'\x7fELF':
            raise ValueError('Not an ELF file')

        self.ei_class = self.data[4]  # 1 = 32-bit, 2 = 64-bit
        self.ei_data = self.data[5]   # 1 = little-endian, 2 = big-endian

        if self.ei_class == 1:
            self.bits = 32
        elif self.ei_class == 2:
            self.bits = 64
        else:
            raise ValueError(f'Unknown ELF class: {self.ei_class}')

        if self.ei_data == 1:
            self.endian = '<'
        elif self.ei_data == 2:
            self.endian = '>'
        else:
            raise ValueError(f'Unknown ELF data encoding: {self.ei_data}')

        self._parse_ehdr()
        self._parse_phdrs()
        self._parse_shdrs()

    def _parse_ehdr(self):
        if self.bits == 64:
            # Elf64_Ehdr
            fmt = self.endian + '16s HHI QQQ I HHHHHH'
            size = struct.calcsize(fmt)
            fields = struct.unpack_from(fmt, self.data, 0)
            (self.e_ident, self.e_type, self.e_machine, self.e_version,
             self.e_entry, self.e_phoff, self.e_shoff, self.e_flags,
             self.e_ehsize, self.e_phentsize, self.e_phnum,
             self.e_shentsize, self.e_shnum, self.e_shstrndx) = fields
        else:
            # Elf32_Ehdr
            fmt = self.endian + '16s HHI III I HHHHHH'
            size = struct.calcsize(fmt)
            fields = struct.unpack_from(fmt, self.data, 0)
            (self.e_ident, self.e_type, self.e_machine, self.e_version,
             self.e_entry, self.e_phoff, self.e_shoff, self.e_flags,
             self.e_ehsize, self.e_phentsize, self.e_phnum,
             self.e_shentsize, self.e_shnum, self.e_shstrndx) = fields

    def _parse_phdrs(self):
        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            if self.bits == 64:
                fmt = self.endian + 'II QQ QQ QQ'
                fields = struct.unpack_from(fmt, self.data, off)
                phdr = {
                    'p_type': fields[0], 'p_flags': fields[1],
                    'p_offset': fields[2], 'p_vaddr': fields[3],
                    'p_paddr': fields[4], 'p_filesz': fields[5],
                    'p_memsz': fields[6], 'p_align': fields[7]
                }
            else:
                fmt = self.endian + 'II III III'
                fields = struct.unpack_from(fmt, self.data, off)
                phdr = {
                    'p_type': fields[0], 'p_offset': fields[1],
                    'p_vaddr': fields[2], 'p_paddr': fields[3],
                    'p_filesz': fields[4], 'p_memsz': fields[5],
                    'p_flags': fields[6], 'p_align': fields[7]
                }
            self.phdrs.append(phdr)

    def _parse_shdrs(self):
        self.shdrs = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            if self.bits == 64:
                fmt = self.endian + 'II QQ QQ II QQ'
                fields = struct.unpack_from(fmt, self.data, off)
                shdr = {
                    'sh_name': fields[0], 'sh_type': fields[1],
                    'sh_flags': fields[2], 'sh_addr': fields[3],
                    'sh_offset': fields[4], 'sh_size': fields[5],
                    'sh_link': fields[6], 'sh_info': fields[7],
                    'sh_addralign': fields[8], 'sh_entsize': fields[9]
                }
            else:
                fmt = self.endian + 'II III III II'
                fields = struct.unpack_from(fmt, self.data, off)
                shdr = {
                    'sh_name': fields[0], 'sh_type': fields[1],
                    'sh_flags': fields[2], 'sh_addr': fields[3],
                    'sh_offset': fields[4], 'sh_size': fields[5],
                    'sh_link': fields[6], 'sh_info': fields[7],
                    'sh_addralign': fields[8], 'sh_entsize': fields[9]
                }
            self.shdrs.append(shdr)

    def get_section_name(self, shdr):
        """Get the name of a section from the string table."""
        if self.e_shstrndx == 0 or self.e_shstrndx >= len(self.shdrs):
            return b''
        strtab = self.shdrs[self.e_shstrndx]
        name_off = strtab['sh_offset'] + shdr['sh_name']
        end = self.data.index(b'\x00', name_off)
        return bytes(self.data[name_off:end])

    def get_pt_load_hash(self):
        """Compute SHA-256 over all PT_LOAD segments."""
        h = hashlib.sha256()
        for phdr in self.phdrs:
            if phdr['p_type'] == PT_LOAD:
                offset = phdr['p_offset']
                size = phdr['p_filesz']
                h.update(bytes(self.data[offset:offset + size]))
        return h.digest()

    def find_note_section(self):
        """Find the .note.dl-secure section index, or -1."""
        for i, shdr in enumerate(self.shdrs):
            if shdr['sh_type'] == SHT_NOTE:
                name = self.get_section_name(shdr)
                if name == b'.note.dl-secure':
                    return i
        return -1


def read_key(keyfile):
    """Read a 32-byte hex-encoded key from a file."""
    with open(keyfile, 'r') as f:
        hex_key = f.read().strip()
    if len(hex_key) != 64:
        print(f'Error: key must be 64 hex characters (32 bytes), '
              f'got {len(hex_key)}', file=sys.stderr)
        sys.exit(1)
    return bytes.fromhex(hex_key)


def compute_hmac(key, data):
    """Compute HMAC-SHA256."""
    return hmac.new(key, data, hashlib.sha256).digest()


def build_note_section(signature):
    """Build the raw bytes for an ELF note containing the signature."""
    name = DL_SECURE_NOTE_NAME
    namesz = len(name)
    descsz = len(signature)
    # Pad name to 4-byte alignment
    name_padded = name + b'\x00' * ((4 - namesz % 4) % 4)
    # Pad desc to 4-byte alignment
    desc_padded = signature + b'\x00' * ((4 - descsz % 4) % 4)

    header = struct.pack('<III', namesz, descsz, DL_SECURE_NOTE_TYPE)
    return header + name_padded + desc_padded


def do_hash(binary_path):
    """Print the SHA-256 hash of PT_LOAD segments."""
    with open(binary_path, 'rb') as f:
        data = f.read()
    elf = ELFFile(data)
    digest = elf.get_pt_load_hash()
    print(f'sha256:{digest.hex()}')


def do_sign(binary_path, key):
    """Sign an ELF binary by adding a .note.dl-secure section."""
    with open(binary_path, 'rb') as f:
        data = f.read()

    elf = ELFFile(data)

    # Check if .note.dl-secure already exists
    existing_idx = elf.find_note_section()
    if existing_idx >= 0:
        print('Warning: replacing existing .note.dl-secure section',
              file=sys.stderr)

    # Build the final file layout first with a PLACEHOLDER (zero)
    # signature.  The ELF header at offset 0 is inside the first
    # PT_LOAD segment, so we must finalize all header modifications
    # before computing the PT_LOAD hash.

    placeholder_sig = b'\x00' * DL_SECURE_HASH_SIZE
    note_data = build_note_section(placeholder_sig)

    out = bytearray(data)

    # Append a new copy of the string table with our section name
    # added (avoids inserting into the middle of the file).
    section_name = b'.note.dl-secure\x00'
    strtab_shdr = elf.shdrs[elf.e_shstrndx]
    strtab_off = strtab_shdr['sh_offset']
    strtab_size = strtab_shdr['sh_size']
    new_name_idx = strtab_size

    old_strtab = bytes(out[strtab_off:strtab_off + strtab_size])
    new_strtab = old_strtab + section_name

    if len(out) % 4 != 0:
        out += b'\x00' * (4 - len(out) % 4)
    new_strtab_off = len(out)
    out += new_strtab

    strtab_shdr['sh_offset'] = new_strtab_off
    strtab_shdr['sh_size'] = len(new_strtab)

    # Append the note section (with placeholder signature).
    note_offset = len(out)
    if note_offset % 4 != 0:
        out += b'\x00' * (4 - note_offset % 4)
        note_offset = len(out)
    out += note_data

    # Record where the signature bytes live inside the note so we
    # can patch them later.
    namesz = len(DL_SECURE_NOTE_NAME)
    aligned_namesz = (namesz + 3) & ~3
    sig_file_offset = note_offset + 12 + aligned_namesz

    # Build section header for .note.dl-secure
    if elf.bits == 64:
        shdr_fmt = elf.endian + 'II QQ QQ II QQ'
    else:
        shdr_fmt = elf.endian + 'II III III II'
    new_shdr = struct.pack(shdr_fmt,
                           new_name_idx, SHT_NOTE, 0, 0,
                           note_offset, len(note_data), 0, 0, 4, 0)

    # Rebuild the entire section header table at the end.
    shdr_table = bytearray()
    for shdr in elf.shdrs:
        shdr_table += struct.pack(
            shdr_fmt,
            shdr['sh_name'], shdr['sh_type'],
            shdr['sh_flags'], shdr['sh_addr'],
            shdr['sh_offset'], shdr['sh_size'],
            shdr['sh_link'], shdr['sh_info'],
            shdr['sh_addralign'], shdr['sh_entsize'])
    shdr_table += new_shdr
    new_e_shnum = elf.e_shnum + 1

    shdr_offset = len(out)
    if shdr_offset % 8 != 0:
        out += b'\x00' * (8 - shdr_offset % 8)
        shdr_offset = len(out)
    out += shdr_table

    # Update the ELF header (this modifies PT_LOAD content at offset 0).
    if elf.bits == 64:
        struct.pack_into(elf.endian + 'Q', out, 40, shdr_offset)
        struct.pack_into(elf.endian + 'H', out, 60, new_e_shnum)
    else:
        struct.pack_into(elf.endian + 'I', out, 32, shdr_offset)
        struct.pack_into(elf.endian + 'H', out, 48, new_e_shnum)

    # NOW compute the PT_LOAD hash over the final file layout.
    final_elf = ELFFile(bytes(out))
    pt_load_hash = final_elf.get_pt_load_hash()
    signature = compute_hmac(key, pt_load_hash)

    # Patch the real signature into the note section.
    out[sig_file_offset:sig_file_offset + DL_SECURE_HASH_SIZE] = signature

    # Write back atomically using a temp file
    dir_name = os.path.dirname(os.path.abspath(binary_path))
    fd, tmp_path = tempfile.mkstemp(dir=dir_name)
    try:
        os.write(fd, bytes(out))
        os.close(fd)
        # Preserve permissions
        st = os.stat(binary_path)
        os.chmod(tmp_path, st.st_mode)
        shutil.move(tmp_path, binary_path)
    except:
        os.close(fd)
        os.unlink(tmp_path)
        raise

    print(f'Signed: {binary_path}')
    print(f'  PT_LOAD hash: {pt_load_hash.hex()}')
    print(f'  HMAC-SHA256:  {signature.hex()}')


def do_verify(binary_path, key):
    """Verify an ELF binary's .note.dl-secure signature."""
    with open(binary_path, 'rb') as f:
        data = f.read()

    elf = ELFFile(data)
    idx = elf.find_note_section()
    if idx < 0:
        print(f'FAIL: no .note.dl-secure section found in {binary_path}',
              file=sys.stderr)
        return False

    shdr = elf.shdrs[idx]
    note_raw = bytes(data[shdr['sh_offset']:
                          shdr['sh_offset'] + shdr['sh_size']])

    # Parse note header
    if len(note_raw) < 12:
        print('FAIL: note section too small', file=sys.stderr)
        return False

    namesz, descsz, ntype = struct.unpack_from('<III', note_raw, 0)
    if ntype != DL_SECURE_NOTE_TYPE:
        print(f'FAIL: unexpected note type 0x{ntype:08x}', file=sys.stderr)
        return False
    if namesz != len(DL_SECURE_NOTE_NAME):
        print('FAIL: unexpected note name size', file=sys.stderr)
        return False

    name_offset = 12
    aligned_namesz = (namesz + 3) & ~3
    desc_offset = name_offset + aligned_namesz

    if desc_offset + descsz > len(note_raw):
        print('FAIL: note section truncated', file=sys.stderr)
        return False

    stored_sig = note_raw[desc_offset:desc_offset + descsz]
    if len(stored_sig) != DL_SECURE_HASH_SIZE:
        print('FAIL: signature wrong size', file=sys.stderr)
        return False

    # Compute expected signature
    pt_load_hash = elf.get_pt_load_hash()
    expected_sig = compute_hmac(key, pt_load_hash)

    if hmac.compare_digest(stored_sig, expected_sig):
        print(f'OK: valid signature for {binary_path}')
        return True
    else:
        print(f'FAIL: signature mismatch for {binary_path}',
              file=sys.stderr)
        print(f'  Expected: {expected_sig.hex()}', file=sys.stderr)
        print(f'  Got:      {stored_sig.hex()}', file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Sign, verify, or hash ELF binaries for dl-secure')
    parser.add_argument('--key', metavar='KEYFILE',
                        help='Path to hex-encoded 32-byte HMAC key file')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--sign', metavar='BINARY',
                       help='Sign an ELF binary')
    group.add_argument('--verify', metavar='BINARY',
                       help='Verify an ELF binary signature')
    group.add_argument('--hash', metavar='BINARY',
                       help='Print SHA-256 hash of PT_LOAD segments')
    args = parser.parse_args()

    if args.hash:
        do_hash(args.hash)
        return

    if not args.key:
        parser.error('--key is required for --sign and --verify')

    key = read_key(args.key)

    if args.sign:
        do_sign(args.sign, key)
    elif args.verify:
        if not do_verify(args.verify, key):
            sys.exit(1)


if __name__ == '__main__':
    main()
