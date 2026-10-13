# Offline sanity test of the ssao.frag GTAO port: renders a synthetic depth
# buffer (floor plane + wall => concave corner) with a D3D-style projection,
# then runs the EXACT shader math (fetchPos, normal reconstruction, GTAO loop)
# per pixel. Expected: visibility ~1 on open floor / wall, a dark band along
# the floor-wall seam. If the open floor reads well below 1 or the seam does
# not darken, the port has a math bug.
import numpy as np

W, H = 160, 100            # AO target (half-res analog)
FW, FH = 320, 200          # "full-res" depth
zn, zf = 0.2, 350.0
fovY = np.radians(67.5)
tanY = np.tan(fovY / 2)
tanX = tanY * (FW / FH)
m33 = zf / (zf - zn)
m43 = -zn * zf / (zf - zn)

cam_pos = np.array([0.0, 1.6, 0.0])
cam_dir = np.array([0.0, -0.35, 1.0]); cam_dir /= np.linalg.norm(cam_dir)
cam_right = np.array([1.0, 0.0, 0.0])
cam_top = np.cross(cam_right, cam_dir) * -1  # y-up-ish
cam_top /= np.linalg.norm(cam_top)
cam_right = np.cross(cam_dir, cam_top); cam_right /= np.linalg.norm(cam_right)
cam_top = np.cross(cam_right, cam_dir)
if cam_top[1] < 0: cam_top = -cam_top

# Scene: floor y=0, wall z=12 (facing camera). Ray-trace depth.
def trace(ro, rd):
    ts = []
    if rd[1] < -1e-6: ts.append((-ro[1]) / rd[1])           # floor y=0
    if rd[2] >  1e-6: ts.append((12.0 - ro[2]) / rd[2])     # wall z=12
    ts = [t for t in ts if t > 0]
    return min(ts) if ts else 1e9

depth = np.ones((FH, FW), dtype=np.float64)
for j in range(FH):
    for i in range(FW):
        u, v = (i + 0.5) / FW, (j + 0.5) / FH
        ndc = np.array([u * 2 - 1, 1 - 2 * v])
        ray = cam_dir + cam_right * tanX * ndc[0] + cam_top * tanY * ndc[1]
        t = trace(cam_pos, ray)
        zview = t  # ray scaled: wp = ro + ray*zview where zview is along-ray param of unnormalized ray
        # zview here = t (param of unnormalized ray) -> view depth = t (matches shader convention)
        if zview < 1e8:
            zndc = m33 + m43 / zview
            depth[j, i] = zndc

def sample_depth(uv):
    x = min(max(int(uv[0] * FW), 0), FW - 1)
    y = min(max(int(uv[1] * FH), 0), FH - 1)
    return depth[y, x]

def fetch_pos(uv):
    zndc = sample_depth(uv)
    zview = np.clip(m43 / (zndc - m33), 0.0, 10000.0)
    ndc = np.array([uv[0] * 2 - 1, 1 - 2 * uv[1]])
    ray = cam_dir + cam_right * tanX * ndc[0] + cam_top * tanY * ndc[1]
    return ray * zview, zview

def fast_acos(v):
    v = np.clip(v, -1, 1)
    res = -0.156583 * abs(v) + np.pi / 2
    res *= np.sqrt(1 - abs(v))
    return res if v >= 0 else np.pi - res

SLICES, NSAMP, RADIUS = 4, 3, 4.0
res = np.array([W, H], float); invres = 1.0 / res
rightU = cam_right; topU = cam_top

def gtao(px, py):
    uv = np.array([(px + 0.5) / W, (py + 0.5) / H])
    zndc = sample_depth(uv)
    if zndc >= 0.9999: return 1.0
    cpos, cw = fetch_pos(uv)
    viewV = -cpos / np.linalg.norm(cpos)
    R, _ = fetch_pos(uv + [invres[0], 0]); Lp, _ = fetch_pos(uv - [invres[0], 0])
    U, _ = fetch_pos(uv + [0, invres[1]]); D, _ = fetch_pos(uv - [0, invres[1]])
    Rw, Lw = np.linalg.norm(R - cpos), np.linalg.norm(cpos - Lp)
    ddx = (R - cpos) if abs(fetch_pos(uv + [invres[0],0])[1] - cw) < abs(cw - fetch_pos(uv - [invres[0],0])[1]) else (cpos - Lp)
    ddy = (U - cpos) if abs(fetch_pos(uv + [0,invres[1]])[1] - cw) < abs(cw - fetch_pos(uv - [0,invres[1]])[1]) else (cpos - D)
    N = np.cross(ddy, ddx); n_ = np.linalg.norm(N)
    if n_ < 1e-9: return 1.0
    N /= n_
    if np.dot(N, viewV) < 0: N = -N
    proj_scale = res[1] / (2 * tanY)
    screen_radius = (RADIUS * 0.5 * proj_scale) / cw
    ip = np.array([px, py], int)
    noiseOffset = 0.25 * ((ip[1] - ip[0]) & 3)
    noiseDirection = (52.9829189 * ((np.dot(ip, [0.06711056, 0.00583715])) % 1)) % 1
    falloff_mul = 2.0 / RADIUS**2
    screen_res_mul = (1.0 / NSAMP) * invres
    vis = 0.0
    for s_ in range(SLICES):
        phi = (s_ + noiseDirection) * np.pi / SLICES
        omega = np.array([np.cos(phi), np.sin(phi)])
        directionV = rightU * omega[0] + topU * omega[1]
        orthoDir = directionV - np.dot(directionV, viewV) * viewV
        axisV = np.cross(directionV, viewV)
        projN = N - axisV * np.dot(N, axisV)
        pl = np.linalg.norm(projN)
        if pl < 1e-9: continue
        sgnN = np.sign(np.dot(orthoDir, projN)) or 1.0
        cosN = np.clip(np.dot(projN, viewV) / pl, 0, 1)
        nang = sgnN * fast_acos(cosN)
        sinN2 = 2 * np.sin(nang)
        for side in range(2):
            ss = -1 + 2 * side
            chc = -1.0
            for k in range(NSAMP):
                s = np.maximum(screen_radius * (k + noiseOffset), 4.0 + k) * screen_res_mul
                st = uv + ss * s * np.array([omega[0], -omega[1]])
                sp, _ = fetch_pos(st)
                hv = sp - cpos
                fall = np.clip(np.dot(hv, hv) * falloff_mul, 0, 1)
                hl = np.linalg.norm(hv)
                Hc = np.dot(hv / hl, viewV) if hl > 1e-9 else -1
                chc = (Hc * (1 - fall) + chc * fall) if Hc > chc else chc
            h = nang + np.clip(ss * fast_acos(chc) - nang, -np.pi/2, np.pi/2)
            vis += pl * (cosN + h * sinN2 - np.cos(2*h - nang)) / 4
    return np.clip(vis / SLICES, 0, 1)

# Sample lines: a column crossing the floor->wall seam, and open floor row.
col = W // 2
print("col x=%d (top=wall ... bottom=near floor):" % col)
for py in range(6, H, 6):
    uv = np.array([(col + 0.5) / W, (py + 0.5) / H])
    _, zv = fetch_pos(uv)
    print("  py=%3d zview=%7.2f vis=%.3f" % (py, zv, gtao(col, py)))
