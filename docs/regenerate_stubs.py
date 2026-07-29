#!/usr/bin/env python3

import os
import sys
import struct

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} [path/to/stubs/folder]")
    sys.exit(1)

destpath = os.path.realpath(sys.argv[1])

if __name__ == '__main__':
    print(f"Writing python stubs to {destpath}")

os.makedirs(destpath, exist_ok=True)
    
# do everything relative to this script
docsdir = os.path.realpath(os.path.dirname(__file__))
    
# path to module libraries for windows
if struct.calcsize("P") == 8:
    binpath = os.path.abspath(os.path.join(docsdir, '../x64/'))
else:
    binpath = os.path.abspath(os.path.join(docsdir, '../Win32/'))

# Prioritise release over development builds
sys.path.insert(0, os.path.abspath(binpath + '/Development/pymodules'))
sys.path.insert(0, os.path.abspath(binpath + '/Release/pymodules'))

# Add the build paths to PATH so noobdawn.dll can be located
os.environ["PATH"] += os.pathsep + os.path.abspath(binpath + '/Development/')
os.environ["PATH"] += os.pathsep + os.path.abspath(binpath + '/Release/')

if sys.platform == 'win32' and sys.version_info[1] >= 8:
    os.add_dll_directory(binpath + '/Release/')
    os.add_dll_directory(binpath + '/Development/')

# path to module libraries for linux
sys.path.insert(0, os.path.abspath(os.path.join(docsdir, '../build/lib')))

import noobdawn
import qnoobdawn

if __name__ == '__main__':
    print(f"Generating stubs from {noobdawn.__file__} and {qnoobdawn.__file__}")

from stubs_generation.helpers import generator3

if __name__ == '__main__':
    generator3.main(['noobdawn', '-d', destpath])
    generator3.main(['qnoobdawn', '-d', destpath])
