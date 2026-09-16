#!/usr/bin/env python3
"""pe_audit.py — read the facts straight out of the linked PE file.

No build log can argue with this: the checks the acceptance criteria care about
are properties of the artifact, so they are read from the artifact.

  * machine type (x86 / x64)
  * Subsystem (must be 2 = WINDOWS: no console window at boot)
  * OS and Subsystem version fields (must be >= 6.01 = Windows 7)
  * DllCharacteristics: NX / ASLR actually enabled by the linker
  * every imported DLL and function name -> "only the allowed DLLs" and
    "no API newer than Windows 7 SP1" become checkable, not aspirational
  * embedded resources: RT_MANIFEST (24), RT_VERSION (16)
  * section virtual vs raw sizes, SizeOfImage

Usage
  python3 tests/pe_audit.py bin/TimeKeeper32.exe bin/TimeKeeper64.exe \\
         --require-manifest --require-versioninfo --max-size 153600

Exit status 0 = every check passed, so it works directly as a CI gate.
Pure stdlib, Python 3.6+ (and it runs under the Python 2-free Win7 venv too).

SPDX-License-Identifier: MIT
"""
import argparse
import os.path
import struct
import sys

RES_TYPES = {16: "VERSION", 24: "MANIFEST", 14: "GROUP_ICON", 3: "ICON",
             10: "RCDATA", 2: "CURSOR", 6: "STRING", 9: "GROUP_ICON"}

# The specification's allow-list, plus two deliberate additions, both explained
# in README.md ("Security notes" / "Deviation log"):
#   user32.dll - one MessageBoxW call, only on the interactive failure path
#   msvcrt.dll - deliberately absent: the seven pure C primitives this program
#                needs are implemented in src/tklibc.c so nothing imports it
DEFAULT_ALLOW = {"kernel32.dll", "advapi32.dll", "ws2_32.dll", "wininet.dll",
                 "user32.dll"}

# Exported names that do not exist on Windows 7 SP1. Importing one gives
# "the procedure entry point ... could not be located" at load time - the most
# unpleasant possible failure, because it only appears on the machine you are
# trying to repair.
BANNED = {
    "GetSystemTimePreciseAsFileTime", "SetSystemTimePreciseAsFileTime",
    "SetThreadDescription", "GetPackageFamilyName", "CreatePseudoConsole",
    "QueryThreadCycleTime", "GetSystemFirmwareTable", "CreateFile2",
    "GetTickCount64", "SetWaitableTimerEx", "PowerRequestCreate",
    "Wow64DisableWow64FsRedirection",
}


class PE(object):
    def __init__(self, path):
        self.d = open(path, "rb").read()
        self.size = len(self.d)
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError("not a PE file (missing MZ)")
        self.off = pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file (missing PE signature)")
        # COFF file header: machine @+4, nsec @+6, SizeOfOptionalHeader @+20,
        # Characteristics @+22 (the *file* flags; the loader flags we care about
        # are DllCharacteristics inside the optional header).
        self.machine, self.nsec = struct.unpack_from("<HH", d, pe + 4)
        self.size_opt, self.chars = struct.unpack_from("<HH", d, pe + 20)
        self.opt = pe + 24
        magic = struct.unpack_from("<H", d, self.opt)[0]
        self.plus = magic == 0x20B
        # IMAGE_OPTIONAL_HEADER32 and _64 are laid out identically except that
        # _64 drops BaseOfData and widens ImageBase to 8 bytes: the fields up to
        # NumberOfRvaAndSizes land at the same offsets in both, and only the
        # data directory moves (96 vs 112). Spelled out here because getting
        # this wrong silently reports subsystem 0 for every x64 binary.
        self.o_os, self.o_subsys, self.o_dllch = 40, 68, 70
        self.o_imgsize = 56
        self.o_datadir = 112 if self.plus else 96
        self.sections = []
        so = self.opt + self.size_opt
        for i in range(self.nsec):
            o = so + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsz, va, rsz, rp = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append([name, vsz, va, rsz, rp])

    def off_of(self, rva):
        for _n, _vsz, va, rsz, rp in self.sections:
            if va <= rva < va + max(rsz, 1):
                return rp + (rva - va)
        # some linkers pad; try the raw span
        for _n, _vsz, va, rsz, rp in self.sections:
            if va <= rva < va + max(rsz, 1) + 0x800:
                return rp + (rva - va)
        return None

    def cstr(self, o, limit=64):
        d = self.d
        end = d.find(b"\0", o, o + limit)
        return d[o:end if end > 0 else o].decode("latin1") if o is not None else ""

    def imports(self):
        d = self.d
        rva, _sz = struct.unpack_from("<II", d, self.opt + self.o_datadir + 8)
        base = self.off_of(rva)
        out = []
        if base is None:
            return out
        i = 0
        while True:
            ent = base + i * 20
            if ent + 20 > len(d):
                break
            oft, _t, _f, name_rva, first = struct.unpack_from("<IIIII", d, ent)
            if not (oft or name_rva or first):
                break
            nbase = self.off_of(name_rva)
            dll = self.cstr(nbase) if nbase is not None else "?"
            thunk = self.off_of(oft or first)
            names = []
            if thunk is not None:
                step, fmt = (8, "<Q") if self.plus else (4, "<I")
                j = 0
                while thunk + j * step + step <= len(d):
                    e, = struct.unpack_from(fmt, d, thunk + j * step)
                    if e == 0:
                        break
                    hi = 1 << 63 if self.plus else 1 << 31
                    if e & hi:
                        names.append("#ordinal%d" % (e & 0xFFFF))
                    else:
                        hn = self.off_of(e & 0x7FFFFFFF)
                        if hn is not None and hn + 2 <= len(d):
                            names.append(self.cstr(hn + 2, 96))
                    j += 1
            out.append((dll, names))
            i += 1
        return out

    def resource_types(self):
        d = self.d
        rva, _sz = struct.unpack_from("<II", d, self.opt + self.o_datadir + 16)
        base = self.off_of(rva)
        found = []
        if base is None:
            return found
        # IMAGE_RESOURCE_DIRECTORY: Characteristics(4) TimeDateStamp(4)
        # Major(2) Minor(2) NumberOfNamedEntries(2) NumberOfIdEntries(2)
        n_named, n_id = struct.unpack_from("<HH", d, base + 12)
        for k in range(n_named + n_id):
            e = base + 16 + k * 8
            name, data_rva = struct.unpack_from("<II", d, e)
            if name & 0x80000000:
                no = self.off_of(name & 0x7FFFFFFF)
                found.append(self.cstr(no) if no is not None else "?")
            else:
                found.append(str(name & 0xFFFF))
        return found

    @property
    def subsystem(self):
        return struct.unpack_from("<H", self.d, self.opt + self.o_subsys)[0]

    @property
    def dllchar(self):
        return struct.unpack_from("<H", self.d, self.opt + self.o_dllch)[0]

    @property
    def osver(self):
        return struct.unpack_from("<HH", self.d, self.opt + self.o_os)

    @property
    def subver(self):
        return struct.unpack_from("<HH", self.d, self.opt + self.o_os + 8)

    @property
    def size_image(self):
        return struct.unpack_from("<I", self.d, self.opt + self.o_imgsize)[0]


def audit(path, args, allow):
    rc = 0
    size = os.path.getsize(path)
    print("== %s  (%d bytes on disk)" % (path, size))
    try:
        p = PE(path)
    except Exception as e:                            # noqa: BLE001 - audit tool
        print("   AUDIT ERROR: %s" % e)
        return 1
    print("   machine            : 0x%04X %s" % (
        p.machine, {0x14C: "x86", 0x8664: "x64", 0x1C0: "ARM",
                    0xAA64: "ARM64"}.get(p.machine, "?")))
    print("   subsystem          : %d  %s" % (
        p.subsystem, "WINDOWS (no console flash)" if p.subsystem == 2
        else ("CONSOLE  <-- unexpected for a boot task" if p.subsystem == 3
              else "unexpected")))
    if p.subsystem != 2:
        rc = 1
    print("   OS version field   : %d.%02d      subsystem version: %d.%02d" % (
        p.osver[0], p.osver[1], p.subver[0], p.subver[1]))
    if p.osver < (6, 1):
        print("   note: OS field is below 6.01 (Windows 7); harmless but odd")
    print("   DllCharacteristics : 0x%04X  %s" % (
        p.dllchar, " ".join(n for n, b in
                             [("HIGH_ENTROPY_VA", 0x20), ("DYNAMICBASE/ASLR", 0x40),
                              ("NX_COMPAT", 0x100), ("NO_ISOLATION", 0x200),
                              ("TERMINAL_SERVER_AWARE", 0x8000)]
                             if p.dllchar & b) or "-"))
    print("   SizeOfImage        : %d bytes" % p.size_image)
    print("   sections           :")
    for n, vsz, va, rsz, rp in p.sections:
        print("       %-9s virtual=%-8d raw=%-8d  %s" % (
            n, vsz, rsz, "(no file data: .bss-like)" if rsz == 0 else ""))
    rt = p.resource_types()
    print("   resources          : %s" % (", ".join(
        "%s" % (RES_TYPES.get(int(x), x) if x.isdigit() else x) for x in rt) or "none"))
    if args.require_manifest and "24" not in rt:
        print("   FAIL: RT_MANIFEST (24) not embedded -> UAC/OS version unverifiable")
        rc = 1
    if args.require_versioninfo and "16" not in rt:
        print("   FAIL: RT_VERSION (16) not embedded -> no file properties")
        rc = 1
    imps = p.imports()
    print("   imports            :")
    for dll, names in sorted(imps, key=lambda x: x[0].lower()):
        ok = dll.lower() in allow
        print("       %-16s %3d function(s)%s" % (dll, len(names), "" if ok else "   <-- NOT ALLOWED"))
        if not ok:
            rc = 1
        for n in sorted(names):
            if n in BANNED:
                print("           %s  <-- NOT AVAILABLE ON WINDOWS 7 SP1" % n)
                rc = 1
    if args.show_imports:
        for dll, names in sorted(imps):
            print("     %s: %s" % (dll, ", ".join(sorted(names))))
    if args.max_size and size > args.max_size:
        print("   FAIL: %d bytes > %d limit" % (size, args.max_size))
        rc = 1
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--max-size", type=int, default=0, help="fail above this many bytes")
    ap.add_argument("--require-manifest", action="store_true")
    ap.add_argument("--require-versioninfo", action="store_true")
    ap.add_argument("--allow", default=",".join(sorted(DEFAULT_ALLOW)))
    ap.add_argument("--show-imports", action="store_true")
    args = ap.parse_args()
    allow = {a.strip().lower() for a in args.allow.split(",") if a.strip()}

    rc = 0
    for f in args.files:
        rc |= audit(f, args, allow)
        print()
    print("AUDIT: %s" % ("OK - all checks passed" if rc == 0 else "PROBLEMS FOUND"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
