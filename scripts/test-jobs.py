"""Exercise background tasks, socket wait, pipe backpressure and PID/FD reuse."""
from pathlib import Path
import re
import runpy
import socket
import tempfile

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
prompt = serial["SHELL"]
with tempfile.TemporaryDirectory() as tmp, socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(10)
    with serial["boot"](Path(tmp)/"jobs.img") as guest:
        def command(text, expected=None):
            return guest.expect(text.encode()+b"\r", prompt, expected or b"")
        def build(name, source):
            for line in source.splitlines():
                command(f"echo '{line}' >> {name}.c")
            command(f"tcc {name}.c -o /bin/{name}")
        build("producer", '''#include <lixin.h>
int main(void){static char b[4096]; int i; for(i=0;i<300;i++) if(lx_write(1,b,4096)!=4096)return 1;return 0;}''')
        build("consumer", '''#include <lixin.h>
#include <stdio.h>
int main(void){int n,total=0; char b[4096]; while((n=lx_read(0,b,sizeof(b)))>0)total+=n;printf("total %d\\n",total);return n<0;}''')
        build("peer", f'''#include <lixin.h>
int main(void){{char c; int fd=lx_tcp_connect("10.0.2.2",{listener.getsockname()[1]});
if(fd<0 || lx_poll(fd,LX_READABLE,0)!=0 || lx_poll(fd,LX_READABLE,9000)!=1)return 1;
if(lx_read(fd,&c,1)!=1 || lx_close(fd))return 2;
return lx_save_file("received",&c,1);}}''')
        build("family", '''#include <stdio.h>
#include <lixin.h>
static volatile int value=501;
int main(int argc,char **argv){int i,p[3];char *args[]={"family","child"};
if(argc==2){if(value!=501)return 8;value=lx_pid();return 17;}
value=77;for(i=0;i<3;i++){p[i]=lx_spawn_async("/bin/family",2,args);
if(p[i]<=0 || p[i]==lx_pid() || (i && p[i]<=p[i-1]))return 2;}
for(i=2;i>=0;i--)if(lx_wait(p[i])!=17 || lx_wait(p[i])!=-1)return 3;
if(value!=77)return 4;puts("private globals and child status verified");return 0;}''')
        build("fds", '''#include <stdio.h>
#include <lixin.h>
int main(void){char b[4]={0,127,1,2};int fd=lx_open_write("binary",0);
if(fd<3 || lx_poll(fd,LX_WRITABLE,0)!=2 || lx_write(fd,b,4)!=4 || lx_close(fd))return 2;
fd=lx_open("binary"); if(fd<3 || lx_poll(fd,LX_READABLE,0)!=1 || lx_read(fd,b,4)!=4 || lx_close(fd))return 3;
if(lx_read(fd,b,1)!=-1 || lx_poll(fd,1,0)!=-1 || b[1]!=127)return 4;
puts("descriptors verified");return 0;}''')
        build("burst", '''#include <stdio.h>
int main(void){volatile unsigned i;puts("cpu-start");for(i=0;i<2000000000u;i++){}puts("cpu-finished");return 0;}''')
        def cpu():
            guest.send(b"cpuinfo\r")
            output = guest.until(prompt)
            busy = re.search(rb"^guest_busy_percent=(\d+)$", output, re.M)
            sample = re.search(rb"^sample_ms=(\d+)$", output, re.M)
            assert busy and sample and b"vcpus=1" in output, output
            assert 250 <= int(sample[1]) and 0 <= int(busy[1]) <= 100, output
            return int(busy[1])

        idle = cpu()
        guest.send(b"burst &\r")
        guest.until(b"cpu-start\r\n")
        guest.send(b"echo interactive\r")
        output=guest.until(b"\ninteractive\r\n")
        assert b"cpu-finished" not in output, "CPU-bound job monopolized the processor"
        guest.until(prompt)
        busy = cpu()
        assert busy > idle + 30, (idle, busy)
        guest.until(b"cpu-finished\r\n")
        build("closepipe", '''#include <lixin.h>
int main(void){char c=65;if(lx_write(1,&c,1)!=1 || lx_close(1))return 1;
while(lx_stat("eof-seen")<0){}return 0;}''')
        build("ack", '''#include <lixin.h>
int main(void){char c; if(lx_read(0,&c,1)!=1 || lx_read(0,&c,1)!=0)return 1;
return lx_save_file("eof-seen","yes",3);}''')
        command("closepipe | ack")
        command("cat eof-seen",b"\nyes")
        command("family",b"private globals and child status verified")
        command("fds",b"descriptors verified")
        command("cat < binary | consumer",b"total 4")
        command("cat binary | cat > binary-copy")
        command("consumer < binary-copy | cat",b"total 4")
        command("peer &", b"[pid ")
        with listener.accept()[0] as sock:
            command("producer | consumer > result")
            command("cat result", b"total 1228800")
            sock.sendall(b"Z")
        def heap():
            guest.send(b"sysinfo\r")
            return int(re.search(rb"heap_used_bytes=(\d+)",guest.until(prompt))[1])
        baseline=heap()
        for _ in range(20):
            command("echo x | cat | wc -l", b"\n1\n")
        assert abs(heap()-baseline)<65536, "process memory accumulated after exit"
        command("cat received", b"\nZ")
        guest.send(b"shutdown\r")
        assert guest.vm.wait(timeout=10)==0
print("Verified guest CPU sampling, socket wait during a 1.2 MB pipeline, binary stream EOF and task reuse.")
