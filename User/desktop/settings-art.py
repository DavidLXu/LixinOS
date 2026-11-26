"""Pack the original settings illustration and symbolic icons into the guest binary."""
import io
from pathlib import Path
import subprocess
import sys

from PIL import Image

svg = Path(__file__).with_suffix(".svg")
image = Image.open(io.BytesIO(subprocess.check_output(["rsvg-convert", svg]))).convert("RGBA")
sky = image.crop((0, 0, 640, 96))
icons = image.getchannel("A").crop((0, 100, 360, 124))
with Path(sys.argv[1]).open("w") as header:
    def array(decl, values):
        header.write(f"static const {decl}[] = {{\n")
        for i in range(0, len(values), 24):
            header.write(",".join(map(str, values[i:i+24])) + ",\n")
        header.write("};\n")
    array("unsigned short skyline", [(r >> 3) << 11 | (g >> 2) << 5 | b >> 3 for r,g,b,a in sky.getdata()])
    array("unsigned char symbols", list(icons.getdata()))
