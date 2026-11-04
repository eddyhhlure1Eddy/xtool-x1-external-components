"""Minimal PE32+ export-table reader (no third-party modules).

parse_exports(path) -> dict with keys:
  dllname   internal name from the export directory
  base      ordinal base
  nfunc     NumberOfFunctions
  nname     NumberOfNames
  names     list of (name, ordinal, rva, forwarder_string_or_None) in name-table order
  imagebase, sections, export_dir (rva, size)
"""
import struct


def parse_exports(path):
    with open(path, 'rb') as f:
        d = f.read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file: %s' % path)
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from('<H', d, opt)[0] != 0x20B:
        raise ValueError('not PE32+: %s' % path)
    imagebase = struct.unpack_from('<Q', d, opt + 24)[0]
    exp_rva, exp_size = struct.unpack_from('<II', d, opt + 112)
    secs = []
    so = opt + optsz
    for _ in range(nsec):
        name = d[so:so + 8].rstrip(b'\0').decode('ascii', 'replace')
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, so + 8)
        secs.append((name, va, vsize, rawptr, rawsize))
        so += 40

    def off(rva):
        for _n, va, vs, rp, rs in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        raise ValueError('rva 0x%x not in any section' % rva)

    def cstr(rva):
        o = off(rva)
        return d[o:d.index(b'\0', o)].decode('ascii')

    res = dict(imagebase=imagebase, sections=secs, export_dir=(exp_rva, exp_size),
               dllname=None, base=0, nfunc=0, nname=0, names=[])
    if exp_rva == 0:
        return res
    e = off(exp_rva)
    (_flags, _ts, _maj, _mnr, name_rva, base, nfunc, nname,
     eat, enpt, eot) = struct.unpack_from('<IIHHIIIIIII', d, e)
    funcs = [struct.unpack_from('<I', d, off(eat) + 4 * i)[0] for i in range(nfunc)]
    names = []
    for i in range(nname):
        nrva = struct.unpack_from('<I', d, off(enpt) + 4 * i)[0]
        idx = struct.unpack_from('<H', d, off(eot) + 2 * i)[0]
        rva = funcs[idx]
        fwd = cstr(rva) if exp_rva <= rva < exp_rva + exp_size else None
        names.append((cstr(nrva), base + idx, rva, fwd))
    res.update(dllname=cstr(name_rva), base=base, nfunc=nfunc, nname=nname, names=names)
    return res


def imported_dlls(path):
    """Names of DLLs in the import directory (normal imports only)."""
    with open(path, 'rb') as f:
        d = f.read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    imp_rva, _imp_size = struct.unpack_from('<II', d, opt + 112 + 8)
    secs = []
    so = opt + optsz
    for _ in range(nsec):
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', d, so + 8)
        secs.append((va, vsize, rawptr, rawsize))
        so += 40

    def off(rva):
        for va, vs, rp, rs in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        raise ValueError('rva 0x%x not in any section' % rva)

    out = []
    if imp_rva == 0:
        return out
    o = off(imp_rva)
    while True:
        ilt, _ts, _fc, name_rva, iat = struct.unpack_from('<IIIII', d, o)
        if ilt == 0 and name_rva == 0 and iat == 0:
            break
        n = off(name_rva)
        out.append(d[n:d.index(b'\0', n)].decode('ascii'))
        o += 20
    return out
