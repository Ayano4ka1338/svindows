#!/usr/bin/env python3
"""Rebuild the bundled Win16 X11 agent with Open Watcom C/16 (WATCOM root)."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
watcom = Path(os.environ.get('WATCOM', root / 'build/watcom')).resolve()
compiler = watcom / 'binl64/wcl'
if not compiler.is_file():
    sys.exit('Set WATCOM to an Open Watcom installation with binl64, h and lib286.')
build = root / 'build/win16'
build.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, WATCOM=str(watcom),
           PATH=str(watcom / 'binl64') + os.pathsep + os.environ['PATH'],
           INCLUDE=str(watcom / 'h/win') + os.pathsep + str(watcom / 'h'))
subprocess.run([str(compiler), '-q', '-bt=windows', '-ms', '-zw', '-os', '-we',
                '-fe=' + str(root / 'user/d/xwin/XWIN.EXE'),
                '-fo=' + str(build / 'xwin.obj'),
                str(root / 'user/d/xwin/XWin.c')], cwd=build, env=env, check=True)
