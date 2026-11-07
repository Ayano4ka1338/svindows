#!/usr/bin/env python3
"""Regression: AF_UNIX drain/refill edges between epoll wait scans."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'kernel/linux/Epoll.c').read_text()
a=source.index('static UINT32 linux_epoll_socket_edges(')
b=source.index('\nUINT32 LinuxEpollPoll(',a)
harness=r'''
#include <assert.h>
#include "kernel/linux/SocketPrivate.h"
'''+source[a:b]+r'''
int main(void) {
 static LINUX_EPOLL_CONTEXT e;
 static LINUX_SOCKET s;
 s.Family=LINUX_AF_UNIX;e.Watches[0].Socket=&s;
 assert(linux_epoll_socket_edges(&e,0,4,TRUE)==4);
 s.ReadSequence++;
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==1);
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==0);
 /* No epoll scan sees the empty state: a new arrival is still an edge. */
 s.ReadSequence++;
 assert(linux_epoll_socket_edges(&e,0,5,FALSE)==1);
 assert(linux_epoll_socket_edges(&e,0,5,FALSE)==1);
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==1);
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==0);
 /* A full send queue was drained before the next wait sampled it. */
 s.WriteSequence++;
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==4);
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==0);
 assert(linux_epoll_socket_edges(&e,0,4,TRUE)==0);
 s.ReadSequence++;
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==1);
 /* epoll MOD re-arms an already-ready descriptor. */
 e.Watches[0].LastReady=0;
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==5);
 /* AF_INET retains its current readiness behavior. */
 s.Family=LINUX_AF_INET;s.ReadSequence++;
 assert(linux_epoll_socket_edges(&e,0,5,TRUE)==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sv-epoll-edge-') as tmp:
    p=Path(tmp);(p/'test.c').write_text(harness)
    subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-O2','-Wall','-Wextra',
                    '-Werror','-Wno-multichar','-I.','-Ikernel/include','-Ikernel/net/lwip',
                    '-Ibuild/lwip/src/include',str(p/'test.c'),'-o',str(p/'test')],cwd=root,check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS EPOLLET drain/refill, write-space, non-consuming poll and rearm transitions')
