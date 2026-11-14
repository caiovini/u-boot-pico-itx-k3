#!/usr/bin/env python3
# SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)

"""
setup.py file for SWIG libfdt
Copyright (C) 2017 Google, Inc.
Written by Simon Glass <sjg@chromium.org>

This script is modified from the upstream version, to fit in with the U-Boot
build system.

Files to be built into the extension are provided in SOURCES
C flags to use are provided in CPPFLAGS
Object file directory is provided in OBJDIR
Version is provided in VERSION

If these variables are not given they are parsed from the Makefiles. This
allows this script to be run stand-alone, e.g.:

    ./pylibfdt/setup.py install [--prefix=...]
"""

import os
import re
import shutil
import subprocess
import sys

try:
    from setuptools import setup, Extension
    from setuptools.command.build_py import build_py as _build_py
    HAVE_SETUPTOOLS = True
except ImportError:
    HAVE_SETUPTOOLS = False
    _build_py = object

try:
    from setuptools import sic
except ImportError:
    pass

srcdir = os.path.dirname(__file__)

with open(os.path.join(srcdir, "../README"), "r") as fh:
    long_description = fh.read()

# Decodes a Makefile assignment line into key and value (and plus for +=)
RE_KEY_VALUE = re.compile(r'(?P<key>\w+) *(?P<plus>[+])?= *(?P<value>.*)$')

def get_top_builddir():
    if '--top-builddir' in sys.argv:
        index = sys.argv.index('--top-builddir')
        sys.argv.pop(index)
        return sys.argv.pop(index)
    else:
        return os.path.join(srcdir, '..')

top_builddir = get_top_builddir()

def ParseMakefile(fname):
    """Parse a Makefile to obtain its variables.

    This collects variable assigments of the form:

        VAR = value
        VAR += more

    It does not pick out := assignments, as these are not needed here. It does
    handle line continuation.

    Returns a dict:
        key: Variable name (e.g. 'VAR')
        value: Variable value (e.g. 'value more')
    """
    makevars = {}
    with open(fname) as fd:
        prev_text = ''  # Continuation text from previous line(s)
        for line in fd.read().splitlines():
          if line and line[-1] == '\\':  # Deal with line continuation
            prev_text += line[:-1]
            continue
          elif prev_text:
            line = prev_text + line
            prev_text = ''  # Continuation is now used up
          m = RE_KEY_VALUE.match(line)
          if m:
            value = m.group('value') or ''
            key = m.group('key')

            # Appending to a variable inserts a space beforehand
            if 'plus' in m.groupdict() and key in makevars:
              makevars[key] += ' ' + value
            else:
              makevars[key] = value
    return makevars

def GetEnvFromMakefiles():
    """Scan the Makefiles to obtain the settings we need.

    This assumes that this script is being run from the top-level directory,
    not the pylibfdt directory.

    Returns:
        Tuple with:
            List of swig options
            Version string
            List of files to build
            List of extra C preprocessor flags needed
            Object directory to use (always '')
    """
    basedir = os.path.dirname(os.path.dirname(os.path.abspath(sys.argv[0])))
    swig_opts = ['-I%s' % basedir]
    makevars = ParseMakefile(os.path.join(basedir, 'Makefile'))
    version = '%s.%s.%s' % (makevars['VERSION'], makevars['PATCHLEVEL'],
                            makevars['SUBLEVEL'])
    makevars = ParseMakefile(os.path.join(basedir, 'libfdt', 'Makefile.libfdt'))
    files = makevars['LIBFDT_SRCS'].split()
    files = [os.path.join(basedir, 'libfdt', fname) for fname in files]
    files.append('libfdt.i')
    cflags = ['-I%s' % basedir, '-I%s/libfdt' % basedir]
    objdir = ''
    return swig_opts, version, files, cflags, objdir


progname = sys.argv[0]
files = os.environ.get('SOURCES', '').split()
cflags = os.environ.get('CPPFLAGS', '').split()
objdir = os.environ.get('OBJDIR')
try:
    version = sic(os.environ.get('VERSION'))
except:
    version = os.environ.get('VERSION')
swig_opts = os.environ.get('SWIG_OPTS', '').split()

# If we were called directly rather than through our Makefile (which is often
# the case with Python module installation), read the settings from the
# Makefile.
if not all((swig_opts, version, files, cflags, objdir)):
    swig_opts, version, files, cflags, objdir = GetEnvFromMakefiles()

if not HAVE_SETUPTOOLS or not shutil.which('swig'):
    cc = os.environ.get('CC', 'cc').split()
    c_files = [f for f in files if f.endswith('.c')]
    so_path = os.path.join(objdir or '.', '_libfdt.so')
    cmd = cc + ['-shared', '-fPIC', '-O2'] + cflags + c_files + ['-o', so_path]
    subprocess.check_call(cmd)

    with open(os.path.join(srcdir, 'libfdt.i_shipped'), 'r') as f:
        i_content = f.read()
    py_start = i_content.index('%pythoncode %{') + len('%pythoncode %{')
    py_end = i_content.index('\n%}', py_start)
    py_body = i_content[py_start:py_end]

    ctypes_header = '''# SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
import ctypes
import os
import struct

_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), '_libfdt.so'))

FDT_FIRST_SUPPORTED_VERSION = 0x02
FDT_LAST_COMPATIBLE_VERSION = 0x10
FDT_LAST_SUPPORTED_VERSION = 0x11

FDT_ERR_NOTFOUND = 1
FDT_ERR_EXISTS = 2
FDT_ERR_NOSPACE = 3
FDT_ERR_BADOFFSET = 4
FDT_ERR_BADPATH = 5
FDT_ERR_BADPHANDLE = 6
FDT_ERR_BADSTATE = 7
FDT_ERR_TRUNCATED = 8
FDT_ERR_BADMAGIC = 9
FDT_ERR_BADVERSION = 10
FDT_ERR_BADSTRUCTURE = 11
FDT_ERR_BADLAYOUT = 12
FDT_ERR_INTERNAL = 13
FDT_ERR_BADNCELLS = 14
FDT_ERR_BADVALUE = 15
FDT_ERR_BADOVERLAY = 16
FDT_ERR_NOPHANDLES = 17
FDT_ERR_BADFLAGS = 18
FDT_ERR_ALIGNMENT = 19
FDT_ERR_MAX = 19

FDT_MAX_PHANDLE = 0xfffffffe
FDT_MAGIC = 0xd00dfeed
FDT_BEGIN_NODE = 0x1
FDT_END_NODE = 0x2
FDT_PROP = 0x3
FDT_NOP = 0x4
FDT_END = 0x9

def _buf(ba):
    if isinstance(ba, bytearray):
        return (ctypes.c_char * len(ba)).from_buffer(ba)
    elif isinstance(ba, bytes):
        return ba
    raise TypeError('bytearray or bytes expected')

def _s(val):
    if isinstance(val, str):
        return val.encode('utf-8')
    return bytes(val)

_lib.fdt_strerror.argtypes = [ctypes.c_int]
_lib.fdt_strerror.restype = ctypes.c_char_p
def fdt_strerror(errval):
    res = _lib.fdt_strerror(errval)
    return res.decode('utf-8') if res else ''

_lib.fdt_check_header.argtypes = [ctypes.c_void_p]
_lib.fdt_check_header.restype = ctypes.c_int
def fdt_check_header(fdt):
    return _lib.fdt_check_header(_buf(fdt))

_lib.fdt_next_node.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.fdt_next_node.restype = ctypes.c_int
def fdt_next_node(fdt, offset, depth):
    d = ctypes.c_int(depth)
    ret = _lib.fdt_next_node(_buf(fdt), offset, ctypes.byref(d))
    return [ret, d.value]

_lib.fdt_first_subnode.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_first_subnode.restype = ctypes.c_int
def fdt_first_subnode(fdt, offset):
    return _lib.fdt_first_subnode(_buf(fdt), offset)

_lib.fdt_next_subnode.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_next_subnode.restype = ctypes.c_int
def fdt_next_subnode(fdt, offset):
    return _lib.fdt_next_subnode(_buf(fdt), offset)

def fdt_magic(fdt):
    return struct.unpack_from('>I', fdt, 0)[0]

def fdt_totalsize(fdt):
    return struct.unpack_from('>I', fdt, 4)[0]

def fdt_off_dt_struct(fdt):
    return struct.unpack_from('>I', fdt, 8)[0]

def fdt_off_dt_strings(fdt):
    return struct.unpack_from('>I', fdt, 12)[0]

def fdt_off_mem_rsvmap(fdt):
    return struct.unpack_from('>I', fdt, 16)[0]

def fdt_version(fdt):
    return struct.unpack_from('>I', fdt, 20)[0]

def fdt_last_comp_version(fdt):
    return struct.unpack_from('>I', fdt, 24)[0]

def fdt_boot_cpuid_phys(fdt):
    return struct.unpack_from('>I', fdt, 28)[0]

def fdt_size_dt_strings(fdt):
    return struct.unpack_from('>I', fdt, 32)[0]

def fdt_size_dt_struct(fdt):
    return struct.unpack_from('>I', fdt, 36)[0]

_lib.fdt_num_mem_rsv.argtypes = [ctypes.c_void_p]
_lib.fdt_num_mem_rsv.restype = ctypes.c_int
def fdt_num_mem_rsv(fdt):
    return _lib.fdt_num_mem_rsv(_buf(fdt))

_lib.fdt_get_mem_rsv.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
_lib.fdt_get_mem_rsv.restype = ctypes.c_int
def fdt_get_mem_rsv(fdt, index):
    addr = ctypes.c_uint64(0)
    size = ctypes.c_uint64(0)
    ret = _lib.fdt_get_mem_rsv(_buf(fdt), index, ctypes.byref(addr), ctypes.byref(size))
    return [ret, addr.value, size.value]

_lib.fdt_address_cells.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_address_cells.restype = ctypes.c_int
def fdt_address_cells(fdt, nodeoffset):
    return _lib.fdt_address_cells(_buf(fdt), nodeoffset)

_lib.fdt_size_cells.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_size_cells.restype = ctypes.c_int
def fdt_size_cells(fdt, nodeoffset):
    return _lib.fdt_size_cells(_buf(fdt), nodeoffset)

_lib.fdt_subnode_offset.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
_lib.fdt_subnode_offset.restype = ctypes.c_int
def fdt_subnode_offset(fdt, parentoffset, name):
    return _lib.fdt_subnode_offset(_buf(fdt), parentoffset, _s(name))

_lib.fdt_path_offset.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.fdt_path_offset.restype = ctypes.c_int
def fdt_path_offset(fdt, path):
    return _lib.fdt_path_offset(_buf(fdt), _s(path))

_lib.fdt_get_name.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.fdt_get_name.restype = ctypes.c_char_p
def fdt_get_name(fdt, nodeoffset):
    lenp = ctypes.c_int(0)
    res = _lib.fdt_get_name(_buf(fdt), nodeoffset, ctypes.byref(lenp))
    if res is None:
        return [None, lenp.value]
    return [res.decode('utf-8'), lenp.value]

_lib.fdt_first_property_offset.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_first_property_offset.restype = ctypes.c_int
def fdt_first_property_offset(fdt, nodeoffset):
    return _lib.fdt_first_property_offset(_buf(fdt), nodeoffset)

_lib.fdt_next_property_offset.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_next_property_offset.restype = ctypes.c_int
def fdt_next_property_offset(fdt, prop_offset):
    return _lib.fdt_next_property_offset(_buf(fdt), prop_offset)

_lib.fdt_string.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_string.restype = ctypes.c_char_p
_lib.fdt_get_property_by_offset.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_lib.fdt_get_property_by_offset.restype = ctypes.c_void_p
def fdt_get_property_by_offset(fdt, prop_offset):
    lenp = ctypes.c_int(0)
    b = _buf(fdt)
    ptr = _lib.fdt_get_property_by_offset(b, prop_offset, ctypes.byref(lenp))
    if not ptr:
        return [None, lenp.value]
    plen, nameoff = struct.unpack_from('>II', ctypes.string_at(ptr, 12), 4)
    name = _lib.fdt_string(b, nameoff).decode('utf-8')
    data = bytearray(ctypes.string_at(ptr + 12, plen))
    return [name, data]

_lib.fdt_getprop.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
_lib.fdt_getprop.restype = ctypes.c_void_p
def fdt_getprop(fdt, nodeoffset, prop_name):
    lenp = ctypes.c_int(0)
    ptr = _lib.fdt_getprop(_buf(fdt), nodeoffset, _s(prop_name), ctypes.byref(lenp))
    if not ptr:
        return [None, lenp.value]
    return [ctypes.string_at(ptr, lenp.value), lenp.value]

_lib.fdt_get_phandle.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_get_phandle.restype = ctypes.c_uint32
def fdt_get_phandle(fdt, nodeoffset):
    return int(_lib.fdt_get_phandle(_buf(fdt), nodeoffset))

_lib.fdt_get_alias.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.fdt_get_alias.restype = ctypes.c_char_p
def fdt_get_alias(fdt, name):
    res = _lib.fdt_get_alias(_buf(fdt), _s(name))
    return res.decode('utf-8') if res is not None else None

_lib.fdt_get_path.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
_lib.fdt_get_path.restype = ctypes.c_int
def fdt_get_path(fdt, nodeoffset, size_hint):
    cbuf = ctypes.create_string_buffer(size_hint)
    ret = _lib.fdt_get_path(_buf(fdt), nodeoffset, cbuf, size_hint)
    return [ret, cbuf.value.decode('utf-8')]

_lib.fdt_parent_offset.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_parent_offset.restype = ctypes.c_int
def fdt_parent_offset(fdt, nodeoffset):
    return _lib.fdt_parent_offset(_buf(fdt), nodeoffset)

_lib.fdt_node_offset_by_phandle.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
_lib.fdt_node_offset_by_phandle.restype = ctypes.c_int
def fdt_node_offset_by_phandle(fdt, phandle):
    return _lib.fdt_node_offset_by_phandle(_buf(fdt), phandle)

_lib.fdt_create_empty_tree.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_create_empty_tree.restype = ctypes.c_int
def fdt_create_empty_tree(data, size):
    return _lib.fdt_create_empty_tree(_buf(data), size)

_lib.fdt_open_into.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
_lib.fdt_open_into.restype = ctypes.c_int
def fdt_open_into(fdt, buf, size):
    return _lib.fdt_open_into(_buf(fdt), _buf(buf), size)

_lib.fdt_pack.argtypes = [ctypes.c_void_p]
_lib.fdt_pack.restype = ctypes.c_int
def fdt_pack(fdt):
    return _lib.fdt_pack(_buf(fdt))

_lib.fdt_add_mem_rsv.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
_lib.fdt_add_mem_rsv.restype = ctypes.c_int
def fdt_add_mem_rsv(fdt, addr, size):
    return _lib.fdt_add_mem_rsv(_buf(fdt), addr, size)

_lib.fdt_del_mem_rsv.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_del_mem_rsv.restype = ctypes.c_int
def fdt_del_mem_rsv(fdt, index):
    return _lib.fdt_del_mem_rsv(_buf(fdt), index)

_lib.fdt_set_name.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
_lib.fdt_set_name.restype = ctypes.c_int
def fdt_set_name(fdt, nodeoffset, name):
    return _lib.fdt_set_name(_buf(fdt), nodeoffset, _s(name))

_lib.fdt_setprop_namelen.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
_lib.fdt_setprop_namelen.restype = ctypes.c_int
def fdt_setprop(fdt, nodeoffset, prop_name, val, length):
    v = bytes(val)
    pname = _s(prop_name)
    return _lib.fdt_setprop_namelen(_buf(fdt), nodeoffset, pname, len(pname), v, length)

def fdt_setprop_u32(fdt, nodeoffset, prop_name, val):
    return fdt_setprop(fdt, nodeoffset, prop_name, struct.pack('>I', val & 0xffffffff), 4)

def fdt_setprop_u64(fdt, nodeoffset, prop_name, val):
    return fdt_setprop(fdt, nodeoffset, prop_name, struct.pack('>Q', val & 0xffffffffffffffff), 8)

_lib.fdt_delprop.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
_lib.fdt_delprop.restype = ctypes.c_int
def fdt_delprop(fdt, nodeoffset, prop_name):
    return _lib.fdt_delprop(_buf(fdt), nodeoffset, _s(prop_name))

_lib.fdt_add_subnode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
_lib.fdt_add_subnode.restype = ctypes.c_int
def fdt_add_subnode(fdt, parentoffset, name):
    return _lib.fdt_add_subnode(_buf(fdt), parentoffset, _s(name))

_lib.fdt_del_node.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_del_node.restype = ctypes.c_int
def fdt_del_node(fdt, nodeoffset):
    return _lib.fdt_del_node(_buf(fdt), nodeoffset)

_lib.fdt_create.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.fdt_create.restype = ctypes.c_int
def fdt_create(buf, bufsize):
    return _lib.fdt_create(_buf(buf), bufsize)

_lib.fdt_finish.argtypes = [ctypes.c_void_p]
_lib.fdt_finish.restype = ctypes.c_int
def fdt_finish(fdt):
    return _lib.fdt_finish(_buf(fdt))

_lib.fdt_resize.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
_lib.fdt_resize.restype = ctypes.c_int
def fdt_resize(fdt, buf, bufsize):
    return _lib.fdt_resize(_buf(fdt), _buf(buf), bufsize)

_lib.fdt_add_reservemap_entry.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64]
_lib.fdt_add_reservemap_entry.restype = ctypes.c_int
def fdt_add_reservemap_entry(fdt, addr, size):
    return _lib.fdt_add_reservemap_entry(_buf(fdt), addr, size)

_lib.fdt_finish_reservemap.argtypes = [ctypes.c_void_p]
_lib.fdt_finish_reservemap.restype = ctypes.c_int
def fdt_finish_reservemap(fdt):
    return _lib.fdt_finish_reservemap(_buf(fdt))

_lib.fdt_begin_node.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_lib.fdt_begin_node.restype = ctypes.c_int
def fdt_begin_node(fdt, name):
    return _lib.fdt_begin_node(_buf(fdt), _s(name))

_lib.fdt_property.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int]
_lib.fdt_property.restype = ctypes.c_int
def fdt_property_stub(fdt, name, val, length):
    v = bytes(val)
    return _lib.fdt_property(_buf(fdt), _s(name), v, length)

def fdt_property_string(fdt, name, string):
    b = _s(string) + b'\\x00'
    return fdt_property_stub(fdt, name, b, len(b))

def fdt_property_u32(fdt, name, val):
    return fdt_property_stub(fdt, name, struct.pack('>I', val & 0xffffffff), 4)

def fdt_property_u64(fdt, name, val):
    return fdt_property_stub(fdt, name, struct.pack('>Q', val & 0xffffffffffffffff), 8)

def fdt_property_cell(fdt, name, val):
    return fdt_property_u32(fdt, name, val)

_lib.fdt_end_node.argtypes = [ctypes.c_void_p]
_lib.fdt_end_node.restype = ctypes.c_int
def fdt_end_node(fdt):
    return _lib.fdt_end_node(_buf(fdt))
'''
    with open(os.path.join(objdir or '.', 'libfdt.py'), 'w') as f:
        f.write(ctypes_header + '\n' + py_body + '\n')
    sys.exit(0)

libfdt_module = Extension(
    '_libfdt',
    sources=files,
    include_dirs=[os.path.join(srcdir, 'libfdt')],
    library_dirs=[os.path.join(top_builddir, 'libfdt')],
    swig_opts=swig_opts,
)

class build_py(_build_py):
    def run(self):
        self.run_command("build_ext")
        return super().run()

setup(
    name='libfdt',
    version=version,
    cmdclass = {'build_py' : build_py},
    author='Simon Glass',
    author_email='sjg@chromium.org',
    description='Python binding for libfdt',
    ext_modules=[libfdt_module],
    package_dir={'': objdir},
    py_modules=['libfdt'],

    long_description=long_description,
    long_description_content_type="text/plain",
    url="https://git.kernel.org/pub/scm/utils/dtc/dtc.git",
    license="GPL-2.0-or-later OR BSD-2-Clause",

    classifiers=[
        "Programming Language :: Python :: 3",
        "Operating System :: OS Independent",
    ],

)
