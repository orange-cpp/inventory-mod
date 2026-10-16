"""Prints the exception and the module it happened in from a Windows minidump (stdlib only)."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, dir_rva = struct.unpack_from('<4sIII', data, 0)
streams = {}
for i in range(nstreams):
    stype, size, rva = struct.unpack_from('<III', data, dir_rva + i * 12)
    streams[stype] = (size, rva)

def wstr(rva):
    n, = struct.unpack_from('<I', data, rva)
    return data[rva + 4: rva + 4 + n].decode('utf-16le')

modules = []
if 4 in streams:  # ModuleListStream
    _, rva = streams[4]
    count, = struct.unpack_from('<I', data, rva)
    for i in range(count):
        base, size, csum, stamp, name_rva = struct.unpack_from('<QIIII', data, rva + 4 + i * 108)
        modules.append((base, size, wstr(name_rva)))

def module_of(addr):
    for base, size, name in modules:
        if base <= addr < base + size:
            return f'{name.split(chr(92))[-1]}+0x{addr - base:X}'
    return 'unknown'

if 6 in streams:  # ExceptionStream
    _, rva = streams[6]
    tid, _pad = struct.unpack_from('<II', data, rva)
    code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', data, rva + 8)
    params = struct.unpack_from('<15Q', data, rva + 8 + 4 + 4 + 8 + 8 + 4 + 4)
    print(f'thread {tid} exception 0x{code:08X} at 0x{addr:X} = {module_of(addr)}')
    print('params:', [hex(p) for p in params[:nparams]])
    # CONTEXT (x86 WOW64 or x64): ThreadContext location follows the exception record
    ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 8 + 152)
    print('context size', ctx_size)
    if ctx_size == 716:  # x86 CONTEXT
        names = ['Edi','Esi','Ebx','Edx','Ecx','Eax','Ebp','Eip','SegCs','EFlags','Esp','SegSs']
        vals = struct.unpack_from('<6I', data, ctx_rva + 0x9C) + struct.unpack_from('<6I', data, ctx_rva + 0xB4)
        for n, v in zip(names, vals):
            print(f'  {n} = 0x{v:08X} {module_of(v)}')
print('modules loaded:', len(modules))
for base, size, name in modules:
    n = name.split(chr(92))[-1].lower()
    if 'inventory' in n:
        print('  ', hex(base), name)
