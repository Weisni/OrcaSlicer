"""Generate synthetic, printer-independent inputs for the stock-Orca comparison."""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import xml.etree.ElementTree as ET
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def box(width: float, depth: float, height: float, origin=(0., 0., 0.)):
    vertices = [(0,0,0),(width,0,0),(width,depth,0),(0,depth,0),
                (0,0,height),(width,0,height),(width,depth,height),(0,depth,height)]
    vertices = [tuple(float(v[i] + origin[i]) for i in range(3)) for v in vertices]
    faces = [(0,2,1),(0,3,2),(4,5,6),(4,6,7),(0,1,5),(0,5,4),
             (1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,0,4),(3,4,7)]
    return vertices, faces


def write_stl(path: Path, parts):
    triangles = [(vertices[a], vertices[b], vertices[c])
                 for vertices, faces in parts for a, b, c in faces]
    with path.open("wb") as out:
        out.write(b"Quack stock-Orca comparison fixture".ljust(80, b" "))
        out.write(struct.pack("<I", len(triangles)))
        for triangle in triangles:
            out.write(struct.pack("<12fH", 0., 0., 0., *sum(triangle, ()), 0))


def write_colored_project(path: Path):
    core = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
    material = "http://schemas.microsoft.com/3dmanufacturing/material/2015/02"
    ET.register_namespace("", core)
    ET.register_namespace("m", material)
    def node(parent, tag, **attrs):
        return ET.SubElement(parent, f"{{{core}}}{tag}", attrs)
    model = ET.Element(f"{{{core}}}model", {"unit": "millimeter", "xml:lang": "en-US"})
    resources = node(model, "resources")
    palette = ET.SubElement(resources, f"{{{material}}}colorgroup", {"id": "3"})
    ET.SubElement(palette, f"{{{material}}}color", {"color": "#27AE60FF"})
    ET.SubElement(palette, f"{{{material}}}color", {"color": "#8E44ADFF"})
    build = node(model, "build")
    for number, offset in ((1, (60.,60.,0.)), (2, (100.,60.,0.))):
        obj = node(resources, "object", id=str(number), type="model", pid="3", pindex=str(number-1))
        mesh = node(obj, "mesh")
        vertices, faces = box(18., 18., 12., offset)
        vnodes, tnodes = node(mesh, "vertices"), node(mesh, "triangles")
        for x,y,z in vertices:
            node(vnodes, "vertex", x=str(x), y=str(y), z=str(z))
        for a,b,c in faces:
            node(tnodes, "triangle", v1=str(a), v2=str(b), v3=str(c))
        node(build, "item", objectid=str(number))
    with ZipFile(path, "w", ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml",
            '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
            '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
        archive.writestr("_rels/.rels",
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
            'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        archive.writestr("3D/3dmodel.model", ET.tostring(model, encoding="utf-8", xml_declaration=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    write_stl(output / "paint-depth-coupon.stl", [box(24.,24.,8.)])
    write_stl(output / "overhang-coupon.stl", [
        box(12.,12.,10.), box(24.,12.,2.,(-6.,0.,10.))])
    write_stl(output / "support-capital.stl", [
        box(40.,40.,2.), box(8.,8.,12.,(16.,16.,1.)), box(40.,40.,2.,(0.,0.,12.))])
    write_colored_project(output / "two-colored-objects.3mf")
    manifest = {p.name: {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                         "bytes": p.stat().st_size}
                for p in output.iterdir() if p.suffix in (".stl", ".3mf")}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "fixtures": len(manifest)}))


if __name__ == "__main__":
    main()
