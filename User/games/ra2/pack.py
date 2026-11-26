"""Pack sprite frames and map-specific terrain into compact canvas textures."""
import json
import struct
import sys
from pathlib import Path
from PIL import Image

folder, output, props, match = map(Path, sys.argv[1:])
def atlas(name):
    meta, rgba = (folder / (name + '.atlas')).read_bytes().split(b'\n', 1)
    h = json.loads(meta)
    size = h['atlasWidth'], h['atlasHeight']
    image = Image.frombytes('RGBA', size, rgba[:size[0]*size[1]*4])
    return h, image, rgba

def crop(im, f):
    return im.crop((f['x'], f['y'], f['x']+f['w'], f['y']+f['h']))

textures = []
for name in json.loads((folder/'types.json').read_text()):
    h, im, planes = atlas(name)
    frames = h['frames']
    building = name.startswith('NA')
    count = 1 if building else min(8,len(frames))
    turret = h.get('turret')
    if turret:
        offset = h['colorBytes'] + h['maskBytes']
        turret_im = Image.frombytes('RGBA',(turret['atlasWidth'],turret['atlasHeight']),
                                    planes[offset:offset+turret['colorBytes']])
    ready = h.get('sequence',{}).get('states',{}).get('ready')
    for direction in range(count):
        sector = direction*len(frames)//count
        if ready:
            sector = ready['start'] + direction*ready['facingStride']
        pic = crop(im,frames[sector])
        if turret:
            top = crop(turret_im,turret['frames'][direction*len(turret['frames'])//count])
            # The voxel turret and hull share a facing and a centered origin.
            pic.alpha_composite(top,((pic.width-top.width)//2,(pic.height-top.height)//2-3))
        if pic.getbbox(): pic = pic.crop(pic.getbbox())
        pic.thumbnail((92,84) if building else (40,38) if name!='E2' else (16,24), Image.Resampling.LANCZOS)
        textures.append((name+str(direction),pic))
h, im, _ = atlas('terrain')
for t in h['tiles']:
    pic = crop(im,t)
    pic = pic.resize((48,24),Image.Resampling.BILINEAR)
    textures.append((f"g{t['tileIndex']}_{t['subTile']}",pic))
features = json.loads(match.read_text())['map'].get('terrain',[])
for name in sorted({obj['type'] for obj in features}):
    path = props / (name + '.png')
    if path.is_file() and path.parent == props:
        with Image.open(path) as image:
            pic = image.convert('RGBA')
        pic = pic.crop(pic.getbbox())
        pic.thumbnail((35,45),Image.Resampling.LANCZOS)
        textures.append(('p'+name,pic))
with output.open('wb') as f:
    f.write(b'LRA2'+struct.pack('<I',len(textures)))
    for name, pic in textures:
        f.write(struct.pack('<24sHH',name.encode(),*pic.size))
        f.write(pic.tobytes())
