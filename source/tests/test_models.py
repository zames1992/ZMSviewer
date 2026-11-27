"""Exercise Explorer's provider DLL on dated and recent copies of the same 3D assets.

Run with Python, Pillow and Blender 5.x: python tests/test_models.py
Fixtures use Blender's factory settings and a temporary directory.
"""
import base64
import gzip
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def exporter(root):
    import bpy
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.mesh.primitive_monkey_add()
    bpy.context.preferences.filepaths.save_version = 0
    for name, compressed in [('original', False), ('zstd', True)]:
        bpy.ops.wm.save_as_mainfile(filepath=str(root / (name + '.blend')), compress=compressed)
    bpy.ops.wm.obj_export(filepath=str(root / 'suzanne.obj'), export_materials=False)
    bpy.ops.wm.stl_export(filepath=str(root / 'suzanne.stl'))
    bpy.ops.wm.ply_export(filepath=str(root / 'suzanne.ply'))
    bpy.ops.export_scene.fbx(filepath=str(root / 'suzanne.fbx'))
    for kind, name in [('GLB', 'suzanne.glb'), ('GLTF_SEPARATE', 'suzanne.gltf')]:
        bpy.ops.export_scene.gltf(filepath=str(root / name), export_format=kind)


def additional_fixtures(root):
    gltf = root / 'suzanne.gltf'
    model = json.loads(gltf.read_text(encoding='utf-8'))
    for buf in model['buffers']:
        encoded = base64.b64encode((root / buf['uri']).read_bytes()).decode('ascii')
        buf['uri'] = 'data:application/octet-stream;base64,' + encoded
    gltf.write_text(json.dumps(model), encoding='utf-8')

    plain = (root / 'original.blend').read_bytes()
    (root / 'gzip.blend').write_bytes(gzip.compress(plain))
    assert plain.startswith(b'BLENDER17')
    p = 17
    while p + 32 <= len(plain):
        code, _, _, size, _ = struct.unpack_from('<4sIQQQ', plain, p)
        if code == b'TEST':
            # Change only the tag, so the loader must draw the actual geometry.
            (root / 'mesh-only.blend').write_bytes(plain[:p] + b'SKIP' + plain[p + 4:])
            break
        p += 32 + size
    else:
        (root / 'mesh-only.blend').write_bytes(plain)

    coords = (-1, 0, 0, 1, 0, 0, 0, 1.5, 0.5)
    def chunk(code, payload):
        return struct.pack('<HI', code, 6 + len(payload)) + payload
    vertices = chunk(0x4110, struct.pack('<H9f', 3, *coords))
    faces = chunk(0x4120, struct.pack('<5H', 1, 0, 1, 2, 0))
    obj = chunk(0x4000, b'Triangle\0' + chunk(0x4100, vertices + faces))
    (root / 'triangle.3ds').write_bytes(chunk(0x4d4d, chunk(0x3d3d, obj)))
    (root / 'triangle.ma').write_text(
        '//Maya ASCII 2020 scene\ncreateNode mesh -n "Triangle";\n'
        'setAttr -s 3 ".vt[0:2]" -type "float3" -1 0 0 1 0 0 0 1.5 0.5;\n'
        'setAttr -s 3 ".ed[0:2]" 0 1 0 1 2 0 2 0 0;\n'
        'setAttr ".fc[0]" -type "polyFaces" f 3 0 1 2;\n', encoding='ascii')
    def iff(code, body):
        return code + struct.pack('>I', len(body)) + body + bytes(-len(body) % 4)
    data = struct.pack('>I9f11I', 9, *coords, 6, 0, 1, 1, 2, 2, 0, 3, 0, 1, 0x60000002)
    mesh = iff(b'MESH', b'mesh\0\0' + data)
    (root / 'triangle.mb').write_bytes(iff(b'FOR4', b'Maya' + iff(b'FOR4', b'DMSH' + mesh)))


def run_tests():
    from PIL import Image
    script = Path(__file__).resolve()
    bin_dir = script.parents[1] / 'bin'
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        subprocess.run(['blender', '-b', '-noaudio', '--factory-startup', '--disable-autoexec',
                        '--python-exit-code', '1', '--python', str(script), '--', 'export', str(root)], check=True)
        additional_fixtures(root)
        rendered = {}
        for path in sorted(root.iterdir()):
            if path.suffix not in {'.blend', '.3ds', '.fbx', '.mb', '.ma', '.obj', '.ply', '.glb', '.gltf', '.stl'}:
                continue
            old = root / 'old' / path.name
            old.parent.mkdir(exist_ok=True)
            shutil.copyfile(path, old)
            os.utime(old, (946684800, 946684800))
            def render(src):
                dest = src.with_name(src.name + '.png')
                subprocess.run([bin_dir / 'ThumbTest.exe', 'dll', bin_dir / 'ZMSviewer.dll', src, dest, '256'], check=True)
                with Image.open(dest) as img:
                    rgba = img.convert('RGBA')
                    assert img.size == (256, 256) and rgba.getbbox() and len(rgba.getcolors(256 * 256)) > 8, src
                    return rgba.tobytes()
            rendered[path.name] = render(path)
            assert render(old) == rendered[path.name], path
            assert old.stat().st_mtime == 946684800
        assert rendered['mesh-only.blend'] == rendered['original.blend']
        assert rendered['zstd.blend'] == rendered['original.blend'] == rendered['gzip.blend']
        assert rendered['suzanne.gltf'] == rendered['suzanne.glb']
        print(f'{len(rendered)} model samples, two file dates each, all pixel comparisons passed')


if __name__ == '__main__':
    if '--' in sys.argv and sys.argv[-2] == 'export':
        exporter(Path(sys.argv[-1]))
    else:
        run_tests()
