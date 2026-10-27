bl_info = {
    "name": "slugia .slug3d Exporter",
    "author": "slugia",
    "version": (0, 2, 0),
    "blender": (4, 0, 0),
    "location": "File > Export > slugia (.slug3d)",
    "description": "Exports semantic vector 3D geometry for slugia",
    "category": "Import-Export",
}

import bpy, json, math
from bpy_extras.io_utils import ExportHelper
from bpy.props import StringProperty, BoolProperty

def v3(v): return [float(v.x), float(v.y), float(v.z)]

def material_color(material):
    rgba = list(material.diffuse_color) if material else [0.32, 0.40, 0.65, 1.0]
    tree = getattr(material, "node_tree", None)
    if tree:
        for node in tree.nodes:
            if node.type == "BSDF_PRINCIPLED":
                base = node.inputs.get("Base Color")
                alpha = node.inputs.get("Alpha")
                if base:
                    rgba = list(base.default_value)
                if alpha:
                    rgba[3] *= float(alpha.default_value)
                break
    def srgb(value):
        value = max(0.0, min(1.0, float(value)))
        return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return [srgb(value) for value in rgba[:3]] + [max(0.0, min(1.0, float(rgba[3])))]

def export_mesh(obj, depsgraph):
    eval_obj = obj.evaluated_get(depsgraph)
    mesh = eval_obj.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    try:
        # Keep polygons/topology intact. Slug decides how/when to lower faces; exporter does
        # not eagerly triangulate or bake screen-space tessellation.
        verts = [v3(v.co) for v in mesh.vertices]
        faces = [[int(i) for i in p.vertices] for p in mesh.polygons]
        sharp = []
        for e in mesh.edges:
            if getattr(e, "use_edge_sharp", False):
                sharp.append([int(e.vertices[0]), int(e.vertices[1])])
        materials = [material_color(material) for material in mesh.materials]
        return {"kind":"mesh","vertices":verts,"faces":faces,"sharp_edges":sharp,
                "materials":materials,"face_materials":[int(p.material_index) for p in mesh.polygons]}
    finally:
        eval_obj.to_mesh_clear()

def export_curve(obj, depsgraph):
    curves=[]
    for spline in obj.data.splines:
        if spline.type=="BEZIER":
            pts=[]
            for p in spline.bezier_points:
                pts.append({"p":v3(p.co),"in":v3(p.handle_left),"out":v3(p.handle_right),
                            "handle_left":p.handle_left_type,"handle_right":p.handle_right_type})
            curves.append({"basis":"bezier","cyclic":bool(spline.use_cyclic_u),"points":pts})
        elif spline.type in {"NURBS","POLY"}:
            pts=[[float(p.co.x),float(p.co.y),float(p.co.z),float(p.co.w)] for p in spline.points]
            curves.append({"basis":spline.type.lower(),"cyclic":bool(spline.use_cyclic_u),
                           "degree":int(spline.order_u-1),"points":pts,
                           "endpoint":bool(getattr(spline,"use_endpoint_u",True)),
                           "bezier_knots":bool(getattr(spline,"use_bezier_u",False))})
    result = {"kind":"curve","splines":curves,
              "bevel_depth":float(obj.data.bevel_depth),"extrude":float(obj.data.extrude)}
    if obj.data.bevel_depth > 0 or obj.data.extrude > 0:
        result["mesh_fallback"] = export_mesh(obj, depsgraph)
    return result

def primitive_hint(obj):
    # Metadata is only a hint. Never pretend an arbitrary edited mesh is analytic.
    name=obj.name.lower()
    if obj.type=="MESH":
        if name.startswith("cube"): return {"type":"box"}
        if name.startswith("sphere") or name.startswith("uvsphere"): return {"type":"sphere"}
        if name.startswith("cylinder"): return {"type":"cylinder"}
        if name.startswith("cone"): return {"type":"cone"}
    return None

class SLUG3D_OT_export(bpy.types.Operator, ExportHelper):
    bl_idname="export_scene.slug3d"
    bl_label="Export slugia .slug3d"
    filename_ext=".slug3d"
    filter_glob:StringProperty(default="*.slug3d", options={"HIDDEN"})
    selected_only:BoolProperty(name="Selected Objects", default=True)

    def execute(self, context):
        dg=context.evaluated_depsgraph_get()
        objects=context.selected_objects if self.selected_only else context.scene.objects
        out={"format":"slug3d","version":1,"coordinate_system":"blender-z-up-rh","color_space":"srgb","objects":[]}
        for obj in objects:
            if obj.type not in {"MESH","CURVE"}: continue
            rec={"name":obj.name,"matrix_world":[list(map(float,row)) for row in obj.matrix_world],
                 "color":material_color(obj.active_material)}
            rec["geometry"]=export_curve(obj,dg) if obj.type=="CURVE" else export_mesh(obj,dg)
            hint=primitive_hint(obj)
            if hint: rec["analytic_hint"]=hint
            out["objects"].append(rec)
        with open(self.filepath,"w",encoding="utf-8") as f:
            json.dump(out,f,separators=(",",":"),ensure_ascii=False,allow_nan=False)
        return {"FINISHED"}

def menu(self, context):
    self.layout.operator(SLUG3D_OT_export.bl_idname, text="slugia Vector 3D (.slug3d)")

def register():
    bpy.utils.register_class(SLUG3D_OT_export)
    bpy.types.TOPBAR_MT_file_export.append(menu)

def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu)
    bpy.utils.unregister_class(SLUG3D_OT_export)

if __name__=="__main__": register()
