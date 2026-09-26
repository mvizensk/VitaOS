# Home's cold-boot video, built on the real PS Vita logo (Wikimedia Commons'
# PlayStation_Vita_logo.svg, a Sony trademark). A light streak crosses the
# dark, the letters flip up one after another, a highlight sweeps them, and
# "PlayStation Vita" glows in underneath. Render:
#   blender -b -P home/boot/logo.py -- <out_dir>        (ONLY=40,90 for stills)
# then home/boot/encode.sh <out_dir> makes boot.mp4.
import bpy, math, sys, os
from mathutils import Vector

out = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "/tmp/boot"
here = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(here, "PlayStation_Vita_logo.svg")
FPS, FRAMES = 30, 120
BG = (13 / 255, 15 / 255, 21 / 255)
ACCENT = (88 / 255, 166 / 255, 255 / 255)
WIDTH = 3.6                                   # the wordmark, in scene units

def lin(c):
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.resolution_x, sc.render.resolution_y = 960, 544
sc.render.fps = FPS
sc.frame_start, sc.frame_end = 1, FRAMES
for engine in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
    try:
        sc.render.engine = engine
        break
    except TypeError:
        pass
try:
    sc.eevee.taa_render_samples = 96
    sc.eevee.use_raytracing = True
    sc.eevee.ray_tracing_options.resolution_scale = "1"
except (AttributeError, TypeError):
    pass
sc.view_settings.view_transform = "Standard"
sc.render.image_settings.file_format = "PNG"
sc.render.filepath = os.path.join(out, "f")

world = bpy.data.worlds.new("w")
sc.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (*lin(BG), 1)

def material(name, color, metallic, rough, emission=None, strength=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Metallic"].default_value = metallic
    b.inputs["Roughness"].default_value = rough
    if emission:
        key = "Emission Color" if "Emission Color" in b.inputs else "Emission"
        b.inputs[key].default_value = (*emission, 1)
        b.inputs["Emission Strength"].default_value = strength
    return m

# ---- the logo, split into letters ----
bpy.ops.import_curve.svg(filepath=SVG)
src = [o for o in bpy.data.objects if o.type == "CURVE"][0]
M = src.matrix_world.copy()
splines = []
for sp in src.data.splines:
    pts = [(M @ p.co, M @ p.handle_left, M @ p.handle_right) for p in sp.bezier_points]
    xs = [p[0].x for p in pts]; ys = [p[0].y for p in pts]
    splines.append({"pts": pts, "cyclic": sp.use_cyclic_u, "box": (min(xs), min(ys), max(xs), max(ys))})
allx0 = min(s["box"][0] for s in splines); allx1 = max(s["box"][2] for s in splines)
ally0 = min(s["box"][1] for s in splines); ally1 = max(s["box"][3] for s in splines)
H = ally1 - ally0
mark = [s for s in splines if s["box"][3] > ally0 + 0.75 * H]          # the big letters reach the top of the art
sub = [s for s in splines if s["box"][3] < ally0 + 0.3 * H]            # "PlayStation(R)Vita" along the bottom
# (the TM sits between the two and is in neither)
mx0 = min(s["box"][0] for s in mark); mx1 = max(s["box"][2] for s in mark)
my0 = min(s["box"][1] for s in mark); my1 = max(s["box"][3] for s in mark)
k = WIDTH / (mx1 - mx0)
cx, cy = (mx0 + mx1) / 2, (my0 + my1) / 2
def to_scene(v):                               # centred on the wordmark, scaled to WIDTH
    return Vector(((v.x - cx) * k, (v.y - cy) * k, 0))

def contains(a, b):
    return a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]
groups = []
for s in sorted(mark, key=lambda s: -(s["box"][2] - s["box"][0]) * (s["box"][3] - s["box"][1])):
    for g in groups:
        if contains(g[0]["box"], s["box"]):
            g.append(s); break
    else:
        groups.append([s])
groups.sort(key=lambda g: g[0]["box"][0])

metal = material("metal", (0.86, 0.89, 0.95), 0.85, 0.2)
glow = material("sub", (13 / 255 * 0.08, 15 / 255 * 0.08, 21 / 255 * 0.08), 0.0, 0.6, (1, 1, 1), 0.0)

def build(name, group, depth, bevel, mat):
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = depth
    cu.bevel_depth = bevel
    cu.bevel_resolution = 3
    pts_all = [to_scene(p[0]) for s in group for p in s["pts"]]
    c = Vector((sum(p.x for p in pts_all) / len(pts_all), sum(p.y for p in pts_all) / len(pts_all), 0))
    for s in group:
        sp = cu.splines.new("BEZIER")
        sp.bezier_points.add(len(s["pts"]) - 1)
        for bp, (co, hl, hr) in zip(sp.bezier_points, s["pts"]):
            bp.handle_left_type = bp.handle_right_type = "FREE"
            bp.co = to_scene(co) - c
            bp.handle_left = to_scene(hl) - c
            bp.handle_right = to_scene(hr) - c
        sp.use_cyclic_u = s["cyclic"]
    cu.materials.append(mat)
    o = bpy.data.objects.new(name, cu)
    sc.collection.objects.link(o)
    o.location = c
    return o

letters = [build("L%d" % i, g, 0.07, 0.008, metal) for i, g in enumerate(groups)]
subtitle = build("sub", sub, 0.004, 0.0, glow)
subtitle.location.y -= 0.12                     # a little more air under the wordmark
bpy.data.objects.remove(src)

# ---- camera, lights (no floor: a glossy one mirrored the blue backlight as a slab) ----

bpy.ops.object.camera_add(location=(0.35, 0.05, 6.4))
cam = bpy.context.object
cam.data.lens = 50
cam.data.dof.use_dof = True
cam.data.dof.focus_distance = 5.8
cam.data.dof.aperture_fstop = 2.8
sc.camera = cam
aim = bpy.data.objects.new("aim", None)
sc.collection.objects.link(aim)
aim.location = (0, 0.12, 0)
cam.rotation_euler = (0, 0, 0)                  # straight on; tracking along world Z rolled the view

def light(loc, energy, color, size, target=aim):
    d = bpy.data.lights.new("l%d" % len(bpy.data.lights), "AREA")
    d.energy, d.color, d.size = energy, color, size
    o = bpy.data.objects.new(d.name, d)
    o.location = loc
    sc.collection.objects.link(o)
    c = o.constraints.new("TRACK_TO"); c.target = target; c.track_axis, c.up_axis = "TRACK_NEGATIVE_Z", "UP_Y"
    return o
light((2.5, 2.2, 4.0), 150, (1, 1, 1), 3.0)                      # key, high right
light((-3.0, 0.8, 3.0), 35, (0.9, 0.94, 1), 3.0)                 # fill
light((0, 0.6, -2.0), 500, lin(ACCENT), 3.0)                     # blue rim from behind
sweep = light((-5, 0.4, 2.2), 0, (1, 0.98, 0.95), 0.4)

# the streak that opens it
bpy.ops.mesh.primitive_plane_add(size=1, location=(-6, 0.05, 0.4))
streak = bpy.context.object
streak.scale = (1.6, 0.012, 1)
streak.data.materials.append(material("streak", (0, 0, 0), 0, 1, (0.75, 0.88, 1), 25.0))

# ---- animation ----
def key(obj, path, frame, value):
    setattr(obj, path, value)
    obj.keyframe_insert(path, frame=frame)

key(streak, "location", 1, (-6, 0.05, 0.4))
key(streak, "location", 22, (6, 0.05, 0.4))
key(streak, "scale", 20, (1.6, 0.012, 1))
key(streak, "scale", 24, (0.001, 0.001, 1))

for i, o in enumerate(letters):
    s = 14 + i * 5                                   # a letter every 1/6 s
    home = o.location.copy()
    key(o, "rotation_euler", s, (math.radians(-88), 0, 0))
    key(o, "location", s, home + Vector((0, -0.25, 0.6)))
    key(o, "rotation_euler", s + 14, (math.radians(6), 0, 0))   # a touch past upright...
    key(o, "location", s + 14, home)
    key(o, "rotation_euler", s + 20, (0, 0, 0))                 # ...and settle
    key(o, "hide_render", 1, True)                               # unseen until its turn
    key(o, "hide_render", s, False)

key(sweep, "location", 62, (-5, 0.4, 2.2))
key(sweep.data, "energy", 60, 0)
key(sweep.data, "energy", 64, 1600)
key(sweep, "location", 96, (5, 0.4, 2.2))
key(sweep.data, "energy", 94, 1600)
key(sweep.data, "energy", 98, 0)

key(subtitle, "hide_render", 1, True)
key(subtitle, "hide_render", 78, False)
emit = glow.node_tree.nodes["Principled BSDF"].inputs["Emission Strength"]
emit.default_value = 0.0; emit.keyframe_insert("default_value", frame=78)
emit.default_value = 1.0; emit.keyframe_insert("default_value", frame=98)

key(cam, "location", 1, (0.25, -0.05, 6.8))    # a slow push in
key(cam, "location", 110, (0.0, -0.12, 6.1))

only = os.environ.get("ONLY")
if only:
    for f in only.split(","):
        sc.frame_set(int(f))
        sc.render.filepath = os.path.join(out, "f%04d" % int(f))
        bpy.ops.render.render(write_still=True)
else:
    bpy.ops.render.render(animation=True)
