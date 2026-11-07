# Generic massing models of the KHSV landmarks (milestone 4 step 4, D-025):
# the right footprint, height and colour, not detail. Footprints were read
# off the NAIP 2023 imagery (to a few metres); heights are estimates, as the
# imagery doesn't give them. Fovea's own work, under the repository's
# licence. Builds each model and exports it as a .glb, run by Blender:
#
#   blender --background --factory-startup --python data/models/khsv_landmarks.py -- <out dir>
#
# Each model's ground line is at height 0 (glTF's y = 0), its front (glTF's
# +Z, Blender's -Y) along the depth; width is across it. Every part reaches
# PLINTH metres below the ground line, so a model placed at the lowest
# ground under its footprint (the compiler's rule, D-026) is never left
# floating where the ground falls away.
#
# Each material names its Fovea material ID (a custom property, exported in
# the glTF's extras): 1 roofs and walls, 2 metal (hangars), 3 glass.

import os
import sys

import bmesh
import bpy

PLINTH = 3.0

MATERIALS = {
    # name: (linear base colour, Fovea material)
    "roof_white": ((0.62, 0.62, 0.60, 1.0), 1),
    "roof_blue": ((0.06, 0.16, 0.38, 1.0), 1),
    "concrete": ((0.42, 0.41, 0.39, 1.0), 1),
    "metal": ((0.55, 0.57, 0.59, 1.0), 2),
    "glass": ((0.05, 0.09, 0.12, 1.0), 3),
}


def box(w, d, h, material):
    """A box `w` wide, `d` deep and `h` tall above the ground line."""
    return ("box", w, d, h, material)


def cylinder(radius, z0, z1, material):
    """An upright cylinder from height z0 to z1 (z0 at or below 0 reaches
    the plinth)."""
    return ("cylinder", radius, z0, z1, material)


# name: parts. Widths across (x), depths front to back (y in Blender).
MODELS = {
    "khsv_terminal": [box(197.0, 128.0, 15.0, "roof_white")],
    "khsv_terminal_hall": [box(180.0, 83.0, 12.0, "roof_blue")],
    "khsv_concourse": [box(37.0, 210.0, 10.0, "roof_blue")],
    "khsv_tower": [
        cylinder(4.0, 0.0, 40.0, "concrete"),
        cylinder(7.0, 40.0, 46.0, "glass"),
        cylinder(7.5, 46.0, 47.5, "concrete"),
    ],
    "khsv_tower_base": [box(40.0, 26.0, 8.0, "concrete")],
    "khsv_hangar_27x105": [box(27.0, 105.0, 12.0, "metal")],
    "khsv_hangar_24x147": [box(24.0, 147.0, 12.0, "metal")],
    "khsv_hangar_27x144": [box(27.0, 144.0, 12.0, "metal")],
    "khsv_hangar_28x166": [box(28.0, 166.0, 12.0, "metal")],
    "khsv_hangar_35x108": [box(35.0, 108.0, 14.0, "metal")],
    "khsv_hangar_73x38": [box(73.0, 38.0, 14.0, "roof_blue")],
    "khsv_hangar_72x100": [box(72.0, 100.0, 16.0, "metal")],
    "khsv_hangar_60x108": [box(60.0, 108.0, 16.0, "metal")],
    "khsv_cargo_42x194": [box(42.0, 194.0, 12.0, "roof_white")],
    "khsv_cargo_53x115": [box(53.0, 115.0, 10.0, "roof_white")],
    "khsv_cargo_70x261": [box(70.0, 261.0, 13.0, "roof_white")],
    "khsv_warehouse": [box(215.0, 261.0, 13.0, "roof_white")],
}


def material(name):
    m = bpy.data.materials.get(name)
    if m is None:
        colour, fovea = MATERIALS[name]
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        m.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = colour
        m.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.9
        m.diffuse_color = colour
        m["fovea_material"] = fovea
    return m


def part_object(name, part):
    bm = bmesh.new()
    kind = part[0]
    if kind == "box":
        _, w, d, h, mat = part
        z0, z1 = -PLINTH, h
        bmesh.ops.create_cube(bm, size=1.0)
        for v in bm.verts:
            v.co.x *= w
            v.co.y *= d
            v.co.z = z0 + (v.co.z + 0.5) * (z1 - z0)
    else:
        _, radius, z0, z1, mat = part
        z0 = -PLINTH if z0 <= 0.0 else z0
        bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=24, radius1=radius,
                              radius2=radius, depth=z1 - z0)
        for v in bm.verts:
            v.co.z += (z0 + z1) / 2
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for p in mesh.polygons:
        p.use_smooth = kind == "cylinder"  # flat faces on boxes, round sides on cylinders
    mesh.materials.append(material(mat))
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def build(name, parts, out_dir):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    for i, part in enumerate(parts):
        part_object(f"{name}_{i}", part)
    bpy.ops.export_scene.gltf(
        filepath=os.path.join(out_dir, name + ".glb"),
        export_format="GLB",
        export_yup=True,
        export_extras=True,
        export_normals=True,
        export_texcoords=False,
        export_materials="EXPORT",
        export_apply=True,
    )


def main():
    out_dir = sys.argv[sys.argv.index("--") + 1]
    os.makedirs(out_dir, exist_ok=True)
    for name, parts in MODELS.items():
        build(name, parts, out_dir)
    print(f"wrote {len(MODELS)} models to {out_dir}")


main()
