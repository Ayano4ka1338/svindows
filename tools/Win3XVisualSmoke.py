#!/usr/bin/env python3
"""Boot an isolated Win3/Xorg image; verify two clients, host movement and close."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import tempfile
import time
from PIL import Image

ROOT=Path(__file__).resolve().parent.parent
CHECKS=ROOT/'build/checks'

def run(command,**kwargs):
    result=subprocess.run(command,cwd=ROOT,stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE,**kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode('utf-8','replace'))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kvm',action='store_true')
    parser.add_argument('--gdb-port',type=int,default=0,help='optional localhost QEMU GDB port')
    parser.add_argument('--timeout',type=float,default=240)
    args=parser.parse_args()
    CHECKS.mkdir(parents=True,exist_ok=True)
    transcript=CHECKS/'win3x-visual-serial.log'
    screenshot=CHECKS/'win3x-independent.png'
    output=bytearray()
    with tempfile.TemporaryDirectory(prefix='win3x-smoke-',dir=CHECKS) as temporary:
        tmp=Path(temporary);iso_dir=tmp/'iso'
        shutil.copytree(ROOT/'build/limine-isodir',iso_dir)
        source=(ROOT/'user/l/startx/StartX.c').read_text()
        source=source.replace('{path, exec_option, shell, 0}',
                              '{path, exec_option, shell, "/c", "win3x", 0}')
        source=source.replace('    start_twm();', '''    if (linux_syscall0(57) == 0) {
        static const char *const probe_args[] = {"/W3XCHK.ELF", 0};
        static const char *const probe_env[] = {"DISPLAY=:0", 0};
        linux_syscall3(59, (long)probe_args[0], (long)probe_args, (long)probe_env);
        linux_syscall3(60, 127, 0, 0);
    }
    start_twm();''')
        (tmp/'StartXProbe.c').write_text(source)
        flags=['gcc','-std=gnu11','-ffreestanding','-fno-builtin','-fno-stack-protector',
               '-fno-pie','-no-pie','-mno-red-zone','-mgeneral-regs-only','-nostdlib',
               '-static','-O2','-Wall','-Wextra','-Werror','-Wl,-e,_start',
               '-Wl,--build-id=none']
        run(flags+['-o',str(tmp/'startx.elf'),str(tmp/'StartXProbe.c'),
                   'user/l/runtime/Memory.c'])
        run(flags+['-o',str(tmp/'probe.elf'),'tools/checks/Win3XProbe.c',
                   'user/l/runtime/Memory.c'])
        fat=iso_dir/'boot/initrd.fat'
        ini=(ROOT/'user/d/windows3/WIN.INI').read_bytes()
        ini=re.sub(rb'(?mi)^run=[^\r\n]*',b'run=CALC.EXE ALARMCLK.EXE',ini,count=1)
        (tmp/'WIN.INI').write_bytes(ini)
        for source,dest in [(tmp/'startx.elf','::STARTX.ELF'),
                            (tmp/'probe.elf','::W3XCHK.ELF'),
                            (tmp/'WIN.INI','::WINDOWS3/WIN.INI')]:
            run(['mcopy','-o','-i',str(fat),str(source),dest])
        iso=tmp/'smoke.iso'
        run(['xorriso','-as','mkisofs','-b','boot/limine/limine-bios-cd.bin',
             '-no-emul-boot','-boot-load-size','4','-boot-info-table',
             '-eltorito-alt-boot','-e','boot/limine/limine-uefi-cd.bin',
             '-no-emul-boot','-isohybrid-gpt-basdat','-o',str(iso),str(iso_dir)])
        run(['build/limine-cli','bios-install',str(iso)])
        qmp_path=tmp/'qmp.sock'
        command=['qemu-system-x86_64']+(['-enable-kvm','-cpu','host'] if args.kvm else [])
        command+=['-m','1024M','-no-reboot','-no-shutdown','-boot','d',
                  '-cdrom',str(iso),'-display','none','-monitor','none','-serial','stdio',
                  '-qmp',f'unix:{qmp_path},server=on,wait=off']
        if args.gdb_port:command+=['-gdb',f'tcp:127.0.0.1:{args.gdb_port}']
        process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout,selectors.EVENT_READ)
                def until(pattern):
                    deadline=time.monotonic()+args.timeout
                    while time.monotonic()<deadline:
                        text=output.decode('utf-8','replace')
                        text=re.sub(r'\[dos\] progress[^\r\n]*[\r\n]+','',text)
                        if '[win3x-check] FAIL' in text:
                            raise RuntimeError(text[text.index('[win3x-check] FAIL'):][-500:])
                        match=re.search(pattern,text)
                        if match:return match
                        if process.poll() is not None:raise RuntimeError('QEMU exited')
                        for key,_ in selector.select(.25):
                            chunk=os.read(key.fileobj.fileno(),65536)
                            if not chunk:raise RuntimeError('serial closed')
                            output.extend(chunk)
                            transcript.write_bytes(output)
                    raise TimeoutError(f'waiting for {pattern}')
                until(r'C:\\>');process.stdin.write(b'startx\n');process.stdin.flush()
                match=until(r'\[win3x-check\] READY x=(\d+) y=(\d+) w=(\d+) h=(\d+)')
                x,y,w,h=map(int,match.groups())
                with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as qmp:
                    qmp.settimeout(10);qmp.connect(str(qmp_path));replies=qmp.makefile('rb')
                    json.loads(replies.readline())
                    for request in [{'execute':'qmp_capabilities'},
                                    {'execute':'screendump','arguments':{'filename':str(tmp/'screen.ppm')}}]:
                        qmp.sendall((json.dumps(request)+'\n').encode())
                        while True:
                            reply=json.loads(replies.readline())
                            if 'error' in reply:raise RuntimeError(reply['error'])
                            if 'return' in reply:break
                img=Image.open(tmp/'screen.ppm');img.save(screenshot)
                if x<700 or x+w>img.width or y+h>img.height:
                    raise RuntimeError(f'moved Calculator outside capture: {(x,y,w,h)}')
                crop=img.crop((x+10,y+25,x+w-10,y+h-10))
                pixels=list(crop.get_flattened_data() if hasattr(crop,'get_flattened_data') else crop.getdata())
                if sum(min(p)>235 for p in pixels)<len(pixels)*.60:
                    raise RuntimeError('moved client lost its independent image')
                until(r'\[win3x-check\] PASS independent windows, move and close')
                print('PASS Win3 Calculator and Alarm Clock: distinct X clients, move beyond VGA, close Clock only')
                print(screenshot)
        finally:
            transcript.write_bytes(output)
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()

if __name__=='__main__':main()
