#!/usr/bin/env python3
"""Check UEFI boot, PS/2 echo and clock progress with 16 GiB and no UART.

Requires OVMF_CODE_4M.fd at the standard Debian path and nm. Guest RAM is
a sparse file, avoiding a 16 GiB anonymous-memory commit on the host.
"""
import subprocess,socket,json,time,struct,tempfile,pathlib
root=pathlib.Path(__file__).resolve().parent.parent
symbols={line.split()[2]:int(line.split()[0],16) for line in subprocess.check_output(['nm','-an',str(root/'build/svindowskrnl.elf')],text=True).splitlines() if len(line.split())==3}
with tempfile.TemporaryDirectory(prefix='sv-no-uart-') as td:
 p=subprocess.Popen(['qemu-system-x86_64','-m','16G','-object',f'memory-backend-file,id=ram,size=16G,mem-path={td}/ram,share=on','-machine','q35,memory-backend=ram','-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-cpu','max','-cdrom',str(root/'build/svindows.iso'),'-boot','d','-display','none','-serial','none','-monitor','none','-qmp',f'unix:{td}/qmp,server=on,wait=off','-no-reboot','-no-shutdown'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
 try:
  for _ in range(100):
   if pathlib.Path(td+'/qmp').exists():break
   if p.poll() is not None:raise RuntimeError(p.stderr.read().decode())
   time.sleep(.1)
  s=socket.socket(socket.AF_UNIX);s.settimeout(10)
  for _ in range(120):
   try: s.connect(td+'/qmp'); break
   except (ConnectionRefusedError,FileNotFoundError):
    if p.poll() is not None: raise RuntimeError(p.stderr.read().decode())
    time.sleep(.5)
  f=s.makefile('rb');f.readline()
  def q(cmd,args=None):
   s.sendall((json.dumps({'execute':cmd,**({'arguments':args} if args else {})})+'\n').encode())
   while True:
    r=json.loads(f.readline())
    if 'error' in r:raise RuntimeError(r)
    if 'return' in r:return r['return']
  q('qmp_capabilities')
  def mem(addr,size):
   result=q('human-monitor-command',{'command-line':f'memsave 0x{addr:x} {size} "{td}/mem"'})
   if result:raise RuntimeError(result)
   return pathlib.Path(td+'/mem').read_bytes()
  def console():
   try: data=mem(symbols['g_console'],24)
   except RuntimeError: return ''
   ptr,w,h,x,y=struct.unpack('<QIIII',data)
   if not ptr or not w or not h:return ''
   return mem(ptr,w*h*2)[::2].decode('latin1')
  deadline=time.monotonic()+120
  while time.monotonic()<deadline:
   if 'C:\\>' in console():break
   time.sleep(.5)
  else:
   q('screendump',{'filename':str(root/'build/checks/uefi-16g-fail.ppm')})
   raise RuntimeError('no shell without COM1: '+q('human-monitor-command',{'command-line':'info registers'}))
  assert mem(symbols['g_serial_ready'],1)==b'\0'
  t1=struct.unpack('<Q',mem(symbols['g_tick_count'],8))[0]
  def type_command(command):
   for c in command + '\n':
    code={' ':'spc','\n':'ret','_':'shift-minus','.':'dot'}.get(c,c.lower())
    q('send-key',{'keys':[{'type':'qcode','data':k} for k in code.split('-')],'hold-time':20})
    time.sleep(.06)
  def expect_output(predicate):
   deadline=time.monotonic()+30
   while time.monotonic()<deadline:
    text=console()
    if predicate(text): return
    time.sleep(.2)
   raise RuntimeError('command failed to complete: '+console())
  for iteration in range(3):
   # Let all workers block before sending the next real PS/2 command.
   before=struct.unpack('<Q',mem(symbols['g_tick_count'],8))[0]
   time.sleep(2)
   after=struct.unpack('<Q',mem(symbols['g_tick_count'],8))[0]
   assert after>before, 'system clock stopped while idle'
   type_command('cls')
   type_command('dir')
   expect_output(lambda text: '1 File(s)' in text and 'CMD.EXE' in text.upper() and 'File Not Found' not in text)
   type_command('echo command_done')
   expect_output(lambda text: text.lower().count('command_done')>=2)
   print('PASS PS/2 dir child process, echo, and clock after idle',iteration+1,flush=True)
  t2=struct.unpack('<Q',mem(symbols['g_tick_count'],8))[0];assert t2>t1
  q('screendump',{'filename':str(root/'build/checks/uefi-16g-ps2.ppm')})
  print('PASS QEMU UEFI / q35 / 16 GiB / no COM1: shell boot, PS/2 echo output, timer advances',t1,t2)
 finally:
  p.terminate();p.wait(timeout=5)
