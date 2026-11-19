"""Check page sizes, UTF-8 layout and guest-side JavaScript over real guest TCP."""
from contextlib import contextmanager
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile
from threading import Thread
import zlib

here = Path(__file__).resolve().parent
harness = runpy.run_path(str(here / "test-user.py"))
limit = 10 * 1024 * 1024


def image_png(color):
    def chunk(tag, data):
        return struct.pack(">I", len(data))+tag+data+struct.pack(">I", zlib.crc32(tag+data))
    header = struct.pack(">IIBBBBB", 40, 24, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", header)
            +chunk(b"IDAT", zlib.compress((b"\0"+color*40)*24))+chunk(b"IEND", b""))


@contextmanager
def site(root):
    (root/"tile.png").write_bytes(image_png(bytes.fromhex("b2396c")))
    for name, fmt, color in (("photo.jpg","jpeg","187cab"),("anim.gif","gif","e3a02a")):
        source=root/"source.png"
        source.write_bytes(image_png(bytes.fromhex(color)))
        subprocess.run(["sips","-s","format",fmt,source,"--out",root/name],
                       check=True,stdout=subprocess.DEVNULL)
    (root/"sum.js").write_text("function sum(a){return a.reduce(function(x,y){return x+y;},0);}")
    (root/"page.html").write_text(
        '<meta charset="utf-8"><title>Before script</title><p>中文排版，繁體。</p>'
        '<p id="target">DOM</p><img src="tile.png" width="80" alt="Test image"><img src="photo.jpg" width="80"><img src="anim.gif" width="80"><script src="sum.js"></script>'
        '<script>document.title="Guest JS"; document.addEventListener("DOMContentLoaded",function(){var el=document.getElementById("target");el.textContent=el.textContent+" <ready>"}); document.write("<p>脚本计算 "+sum([3,5,8])+"</p>");</script>'
        '<p>'+"山河"*66+'</p>', encoding="utf-8")
    prefix, suffix = b"<title>Boundary</title><pre>", b"</pre><p id=last>End of ten MiB</p>"
    row = b"This line is visible in the ten MiB document. " + b"0123456789"*3 + b"\n"
    fill = limit-len(prefix)-len(suffix)
    page = prefix+(row*(fill//len(row)+1))[:fill]+suffix
    (root/"limit.html").write_bytes(page)
    (root/"large.html").write_bytes(page+b"x")
    (root/"payload.json").write_text('{"message":"Server 42"}')
    (root/"label.js").write_text("if (!document.currentScript) throw Error(); function labelText(data){return data.message;}")
    (root/"events.html").write_text("""<title>Waiting</title><button>Load data</button><p id="result">Pending</p>
<script>
document.cookie="guest=ready; Path=/";
var library=document.createElement("script");library.src="label.js";document.head.appendChild(library);
document.querySelector("button").addEventListener("click",function(){
    var count=0, interval=setInterval(function(){
        if (++count!==2) return;
        clearInterval(interval);
        setTimeout(function(){
            fetch("payload.json").then(function(r){return r.json()}).then(function(data){
                var result=document.querySelector("#result"), label=document.createElement("span");
                label.textContent=labelText(data);
                result.innerHTML="";
                result.appendChild(label);
                document.title=data.message;
            });
        },75);
    },25);
});
</script>""")
    (root/"loop.html").write_text("<p>Still here</p><script>while(true){}</script>")
    class Handler(SimpleHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/payload.json" and "guest=ready" not in self.headers.get("Cookie","").split("; "):
                self.send_error(403)
                return
            super().do_GET()

    with ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler,directory=str(root))) as server:
        Thread(target=server.serve_forever, daemon=True).start()
        try:
            yield f"http://10.0.2.2:{server.server_port}"
        finally:
            server.shutdown()


def serial_test(root, url):
    with harness["boot"](root/"disk.img") as guest:
        for path, expected in [
                ("page.html", ["Guest JS","DOM <ready>","中文排版，繁體。","脚本计算 16"]),
                ("loop.html", ["Still here","execution timeout"]),
                ("limit.html#last", ["End of ten MiB"])]:
            guest.send(f"links {url}/{path}\r".encode())
            result = guest.until(b"Q: quit", timeout=90)
            assert all(s.encode() in result for s in expected), result
            if path == "page.html":
                assert result.count("山".encode()) == 66, result
            guest.expect(b"q", harness["SHELL"])
        guest.send(f"links {url}/large.html\r".encode())
        assert b"exceeds 10 MiB" in guest.until(harness["SHELL"],timeout=90)
        guest.send(b"shutdown\r")
        assert guest.vm.wait(timeout=10)==0


if __name__=="__main__":
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        with site(root) as url:
            serial_test(root,url)
    print("Verified 10 MiB, overflow rejection, UTF-8, external and inline JavaScript, and script timeout.")
