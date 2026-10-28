#!/usr/bin/env python3
"""Verify the imported NVIDIA 595.84 Linux userspace closure.

The checker is deliberately independent of the host dynamic linker.  It
parses the ELF headers, DT_NEEDED entries, dynamic symbol tables, and x86-64
relocations directly so a Windows host can validate the exact tree that will
be copied to the System FAT32 volume.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


EM_X86_64 = 62
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
ET_DYN = 3
PT_LOAD = 1
PT_DYNAMIC = 2
SHT_RELA = 4
SHT_DYNSYM = 11
SHT_REL = 9
SHN_UNDEF = 0

# The complete x86-64 relocation number range currently assigned by the ELF
# ABI and GNU toolchain.  Keeping this explicit catches a wrong-architecture
# or vendor-private relocation without rejecting TLS/GOT relocations used by
# glibc and NVIDIA's official objects.
KNOWN_X86_64_RELOCS = set(range(43)) | set(range(250, 256))


class CheckError(RuntimeError):
    pass


@dataclass
class ProgramHeader:
    p_type: int
    p_offset: int
    p_vaddr: int
    p_filesz: int


class Elf64:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < 64 or self.data[:4] != b"\x7fELF":
            raise CheckError(f"not ELF: {path}")
        ident = self.data[:16]
        if ident[4] != ELFCLASS64 or ident[5] != ELFDATA2LSB:
            raise CheckError(f"not little-endian ELF64: {path}")
        (
            _ident,
            self.e_type,
            self.e_machine,
            _version,
            _entry,
            self.e_phoff,
            self.e_shoff,
            _flags,
            _ehsize,
            self.e_phentsize,
            self.e_phnum,
            self.e_shentsize,
            self.e_shnum,
            self.e_shstrndx,
        ) = struct.unpack_from("<16sHHIQQQIHHHHHH", self.data, 0)
        if self.e_machine != EM_X86_64:
            raise CheckError(f"ELF machine is not x86-64: {path}")
        if self.e_type not in (ET_EXEC, ET_DYN):
            raise CheckError(f"unsupported ELF type {self.e_type}: {path}")
        self.programs = self._program_headers()

    def _program_headers(self) -> list[ProgramHeader]:
        result = []
        for index in range(self.e_phnum):
            offset = self.e_phoff + index * self.e_phentsize
            if offset + 56 > len(self.data):
                raise CheckError(f"truncated program headers: {self.path}")
            p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _align = (
                struct.unpack_from("<IIQQQQQQ", self.data, offset)
            )
            result.append(ProgramHeader(p_type, p_offset, p_vaddr, p_filesz))
        return result

    def vaddr_to_offset(self, address: int) -> int:
        for program in self.programs:
            if program.p_type != PT_LOAD:
                continue
            if program.p_vaddr <= address < program.p_vaddr + program.p_filesz:
                return program.p_offset + (address - program.p_vaddr)
        raise CheckError(f"unmapped dynamic address 0x{address:x}: {self.path}")

    def _dynamic(self) -> dict[int, list[int]]:
        values: dict[int, list[int]] = {}
        for program in self.programs:
            if program.p_type != PT_DYNAMIC:
                continue
            end = program.p_offset + program.p_filesz
            for offset in range(program.p_offset, end, 16):
                if offset + 16 > len(self.data):
                    raise CheckError(f"truncated PT_DYNAMIC: {self.path}")
                tag, value = struct.unpack_from("<qQ", self.data, offset)
                if tag == 0:
                    break
                values.setdefault(tag, []).append(value)
        return values

    def _cstring(self, offset: int) -> str:
        if offset < 0 or offset >= len(self.data):
            raise CheckError(f"invalid ELF string offset: {self.path}")
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise CheckError(f"unterminated ELF string: {self.path}")
        return self.data[offset:end].decode("utf-8", errors="replace")

    def needed(self) -> list[str]:
        dynamic = self._dynamic()
        if 5 not in dynamic:
            return []
        string_table = self.vaddr_to_offset(dynamic[5][0])
        return [self._cstring(string_table + value) for value in dynamic.get(1, [])]

    def _section_headers(self):
        result = []
        if self.e_shoff == 0 or self.e_shnum == 0:
            return result
        for index in range(self.e_shnum):
            offset = self.e_shoff + index * self.e_shentsize
            if offset + 64 > len(self.data):
                raise CheckError(f"truncated section headers: {self.path}")
            result.append(struct.unpack_from("<IIQQQQIIQQ", self.data, offset))
        return result

    def relocation_types(self) -> list[int]:
        relocations = []
        for section in self._section_headers():
            sh_type = section[1]
            if sh_type not in (SHT_RELA, SHT_REL):
                continue
            sh_offset, sh_size, sh_entsize = section[4], section[5], section[9]
            if sh_entsize == 0:
                sh_entsize = 24 if sh_type == SHT_RELA else 16
            if sh_entsize < (24 if sh_type == SHT_RELA else 16):
                raise CheckError(f"invalid relocation entry size: {self.path}")
            for offset in range(sh_offset, sh_offset + sh_size, sh_entsize):
                if offset + 16 > len(self.data):
                    raise CheckError(f"truncated relocation table: {self.path}")
                _address, info = struct.unpack_from("<QQ", self.data, offset)
                relocations.append(info & 0xFFFFFFFF)
        return relocations

    def dynamic_symbols(self) -> tuple[set[str], set[str]]:
        """Return (defined, non-weak undefined) dynamic symbol names."""
        sections = self._section_headers()
        defined: set[str] = set()
        undefined: set[str] = set()
        for section in sections:
            if section[1] != SHT_DYNSYM:
                continue
            offset, size, link, entsize = section[4], section[5], section[6], section[9]
            if entsize == 0:
                entsize = 24
            if link >= len(sections):
                raise CheckError(f"invalid dynsym string-table link: {self.path}")
            string_section = sections[link]
            string_offset, string_size = string_section[4], string_section[5]
            for entry in range(offset, offset + size, entsize):
                if entry + 24 > len(self.data):
                    raise CheckError(f"truncated dynsym: {self.path}")
                name_offset, info, _other, shndx, _value, _size = struct.unpack_from(
                    "<IBBHQQ", self.data, entry
                )
                if name_offset >= string_size:
                    continue
                name_end = self.data.find(b"\0", string_offset + name_offset,
                                           string_offset + string_size)
                if name_end < 0:
                    continue
                name = self.data[string_offset + name_offset:name_end].decode(
                    "utf-8", errors="replace"
                )
                if not name:
                    continue
                binding = info >> 4
                if shndx == SHN_UNDEF:
                    if binding != 2:  # STB_WEAK
                        undefined.add(name)
                else:
                    defined.add(name)
        return defined, undefined


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def norm_relative(value: str) -> str:
    return value.replace("\\", "/").lstrip("/")


def load_manifest(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise CheckError(f"cannot read manifest {path}: {error}") from error
    if not isinstance(value, dict):
        raise CheckError("userspace manifest is not an object")
    if value.get("release") != "595.84":
        raise CheckError("userspace manifest release is not 595.84")
    return value


def verify_manifest(root: Path, manifest_path: Path, package_root: Path | None):
    manifest = load_manifest(manifest_path)
    files = manifest.get("files")
    aliases = manifest.get("aliases")
    if not isinstance(files, list) or not isinstance(aliases, list):
        raise CheckError("userspace manifest has no files/aliases arrays")
    for entry in files:
        target = norm_relative(str(entry["target"]))
        path = root / Path(target)
        if not path.is_file():
            raise CheckError(f"missing staged userspace file: {target}")
        actual = sha256(path)
        if actual != str(entry["sha256"]).upper():
            raise CheckError(f"staged hash mismatch: {target}")
        if path.stat().st_size != int(entry["size"]):
            raise CheckError(f"staged size mismatch: {target}")
        if package_root is not None and entry.get("source_kind") == "nvidia":
            source = package_root / Path(norm_relative(str(entry["source"])))
            if not source.is_file() or sha256(source) != actual:
                raise CheckError(f"NVIDIA source mismatch: {entry['source']}")
    for entry in aliases:
        alias = root / Path(norm_relative(str(entry["alias"])))
        target = root / Path(norm_relative(str(entry["target"])))
        if not alias.is_file() or not target.is_file():
            raise CheckError(f"missing materialized symlink alias: {entry}")
        if sha256(alias) != sha256(target):
            raise CheckError(f"symlink alias content mismatch: {entry['alias']}")
    required_aliases = {
        norm_relative(str(entry["alias"])) for entry in aliases
    }
    expected_aliases = {
        "lib/x86_64-linux-gnu/libEGL.so.1",
        "lib/x86_64-linux-gnu/libEGL.so",
        "lib/x86_64-linux-gnu/libEGL_nvidia.so.0",
        "lib/x86_64-linux-gnu/libOpenGL.so",
        "lib/x86_64-linux-gnu/libnvidia-allocator.so.1",
        "lib/x86_64-linux-gnu/libnvidia-allocator.so",
    }
    if not expected_aliases.issubset(required_aliases):
        raise CheckError("required GLVND symlink topology is incomplete")
    return manifest


def verify_app(root: Path, manifest_path: Path, package_root: Path | None = None):
    """Verify a flattened guest bundle against the unchanged host import manifest."""
    manifest = load_manifest(manifest_path)
    files, aliases = manifest.get("files"), manifest.get("aliases")
    if not isinstance(files, list) or not isinstance(aliases, list):
        raise CheckError("userspace manifest has no files/aliases arrays")
    prefix = "lib/x86_64-linux-gnu/"
    checked = set()
    for entry in files:
        target = norm_relative(str(entry["target"]))
        if not target.startswith(prefix):
            continue  # Host-only loader duplicate and external-platform manifests.
        name = target[len(prefix):]
        if not name or "/" in name or name in (".", ".."):
            raise CheckError(f"invalid library target: {target}")
        path = root / "lib" / name
        if (not path.is_file() or sha256(path) != str(entry["sha256"]).upper()
                or path.stat().st_size != int(entry["size"])):
            raise CheckError(f"app library mismatch: {name}")
        if package_root is not None and entry.get("source_kind") == "nvidia":
            source = package_root / norm_relative(str(entry["source"]))
            if not source.is_file() or sha256(source) != sha256(path):
                raise CheckError(f"NVIDIA source mismatch: {name}")
        checked.add(name)
    required = {"ld-linux-x86-64.so.2", "libc.so.6", "libEGL.so.1.1.0",
                "libEGL_nvidia.so.595.84"}
    if not required.issubset(checked):
        raise CheckError("app library closure is incomplete")
    checked_aliases = set()
    for entry in aliases:
        alias, target = (norm_relative(str(entry[k])) for k in ("alias", "target"))
        if not alias.startswith(prefix) or not target.startswith(prefix):
            raise CheckError("unexpected app alias topology")
        alias, target = alias[len(prefix):], target[len(prefix):]
        if "/" in alias + target or alias in (".", "..") or target in (".", ".."):
            raise CheckError("invalid app alias")
        a, b = root / "lib" / alias, root / "lib" / target
        if not a.is_file() or not b.is_file() or sha256(a) != sha256(b):
            raise CheckError(f"app alias mismatch: {alias}")
        checked_aliases.add(alias)
    if not {"libEGL.so.1", "libEGL.so", "libEGL_nvidia.so.0", "libOpenGL.so",
            "libnvidia-allocator.so.1", "libnvidia-allocator.so"}.issubset(checked_aliases):
        raise CheckError("required GLVND symlink topology is incomplete")
    vendor = json.loads((root / "egl/10_nvidia.json").read_text(encoding="utf-8-sig"))
    expected = f"/usr/apps/{root.name}/lib/libEGL_nvidia.so.0"
    if (vendor.get("file_format_version") != "1.0.0" or
            vendor.get("ICD", {}).get("library_path") != expected):
        raise CheckError("EGL manifest does not point to the app-private ICD")
    license = root / "licenses/NVIDIA-LICENSE"
    if not license.is_file() or license.stat().st_size == 0:
        raise CheckError("missing app NVIDIA license")
    if package_root is not None and sha256(license) != sha256(package_root / "LICENSE"):
        raise CheckError("app NVIDIA license mismatch")
    return manifest


def verify_elves(root: Path):
    elves = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        try:
            if path.read_bytes()[:4] != b"\x7fELF":
                continue
            elves.append((path, Elf64(path)))
        except CheckError:
            raise
        except OSError as error:
            raise CheckError(f"cannot inspect {path}: {error}") from error
    if not elves:
        raise CheckError("userspace tree contains no ELF files")
    by_name: dict[str, list[Path]] = {}
    for path, _elf in elves:
        by_name.setdefault(path.name, []).append(path)
    unresolved = []
    unknown_relocations = []
    all_defined: set[str] = set()
    all_undefined: list[tuple[Path, str]] = []
    for path, elf in elves:
        for needed in elf.needed():
            if needed not in by_name:
                unresolved.append(f"{path.relative_to(root)} -> {needed}")
        for relocation in elf.relocation_types():
            if relocation not in KNOWN_X86_64_RELOCS:
                unknown_relocations.append(
                    f"{path.relative_to(root)} -> R_X86_64_{relocation}"
                )
        defined, undefined = elf.dynamic_symbols()
        all_defined.update(defined)
        all_undefined.extend((path, name) for name in undefined)
    if unresolved:
        raise CheckError("unresolved DT_NEEDED: " + "; ".join(unresolved[:8]))
    if unknown_relocations:
        raise CheckError("unknown x86-64 relocation: " + "; ".join(unknown_relocations[:8]))
    missing_symbols = [
        f"{path.relative_to(root)} -> {name}"
        for path, name in all_undefined
        if name not in all_defined
    ]
    if missing_symbols:
        raise CheckError("unresolved dynamic symbol: " + "; ".join(missing_symbols[:8]))
    return len(elves)


def main() -> int:
    parser = argparse.ArgumentParser()
    roots = parser.add_mutually_exclusive_group(required=True)
    roots.add_argument("--root", type=Path)
    roots.add_argument("--app-root", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--package-root", type=Path)
    args = parser.parse_args()
    try:
        root = (args.app_root or args.root).resolve()
        manifest_path = args.manifest.resolve()
        if not root.is_dir():
            raise CheckError(f"missing userspace root: {root}")
        if not manifest_path.is_file():
            raise CheckError(f"missing userspace manifest: {manifest_path}")
        verify = verify_app if args.app_root else verify_manifest
        manifest = verify(root, manifest_path, args.package_root.resolve()
                                   if args.package_root else None)
        package_run = manifest.get("package_run")
        if package_run and manifest.get("package_sha256"):
            package_path = Path(package_run)
            if package_path.is_file() and sha256(package_path) != str(
                    manifest["package_sha256"]).upper():
                raise CheckError("official 595.84 package hash changed")
        elf_count = verify_elves(root / "lib" if args.app_root else root)
    except (CheckError, OSError, ValueError, KeyError) as error:
        print(f"CHECK_USERSPACE_59584=FAIL: {error}", file=sys.stderr)
        return 1
    print("RELEASE_CONSISTENCY=PASS")
    print("SYMLINK_TOPOLOGY=PASS")
    print(f"ELF_X86_64_COUNT={elf_count}")
    print("DT_NEEDED_GATE=PASS")
    print("DYNAMIC_SYMBOL_GATE=PASS")
    print("RELOCATION_GATE=PASS")
    print("CHECK_USERSPACE_59584=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
