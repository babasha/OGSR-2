// Offline sanity test of the ssao.frag GTAO port (see gtao_selftest.py note).
// Synthetic scene: floor y=0 + wall z=12 (concave seam). Runs the exact shader
// math per pixel. Expect vis~1 on open floor/wall, dark band at the seam.
const W = 160, H = 100;        // AO target (half-res analog)
const FW = 320, FH = 200;      // full-res depth
const zn = 0.2, zf = 350.0;
const fovY = 67.5 * Math.PI / 180;
const tanY = Math.tan(fovY / 2), tanX = tanY * (FW / FH);
const m33 = zf / (zf - zn), m43 = -zn * zf / (zf - zn);

const v3 = (x, y, z) => [x, y, z];
const add = (a, b) => [a[0]+b[0], a[1]+b[1], a[2]+b[2]];
const sub = (a, b) => [a[0]-b[0], a[1]-b[1], a[2]-b[2]];
const mul = (a, s) => [a[0]*s, a[1]*s, a[2]*s];
const dot = (a, b) => a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const len = a => Math.hypot(a[0], a[1], a[2]);
const norm = a => mul(a, 1 / len(a));
const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);

const camPos = v3(0, 1.6, 0);
let camDir = norm(v3(0, -0.35, 1));
let camRight = norm(cross(v3(0, 1, 0), camDir));          // world-up x dir
let camTop = norm(cross(camDir, camRight));
if (camTop[1] < 0) camTop = mul(camTop, -1);

// Depth buffer: ray-trace floor + wall. zview convention = scale of the
// UNNORMALIZED frustum ray (camDir + right*tanX*x + top*tanY*y), i.e. the
// view-space z — so solve along the ray for each surface.
const depth = new Float64Array(FW * FH).fill(1.0);
for (let j = 0; j < FH; ++j) for (let i = 0; i < FW; ++i) {
    const u = (i + 0.5) / FW, v = (j + 0.5) / FH;
    const ndc = [u * 2 - 1, 1 - 2 * v];
    const ray = add(camDir, add(mul(camRight, tanX * ndc[0]), mul(camTop, tanY * ndc[1])));
    let t = Infinity;
    if (ray[1] < -1e-6) t = Math.min(t, -camPos[1] / ray[1]);       // floor y=0
    if (ray[2] >  1e-6) t = Math.min(t, (12 - camPos[2]) / ray[2]); // wall z=12
    if (isFinite(t) && t > 0) depth[j * FW + i] = m33 + m43 / t;
}

const sampleDepth = uv => {
    const x = clamp(Math.floor(uv[0] * FW), 0, FW - 1);
    const y = clamp(Math.floor(uv[1] * FH), 0, FH - 1);
    return depth[y * FW + x];
};
const fetchPos = uv => {
    const zndc = sampleDepth(uv);
    const zview = clamp(m43 / (zndc - m33), 0, 10000);
    const ndc = [uv[0] * 2 - 1, 1 - 2 * uv[1]];
    const ray = add(camDir, add(mul(camRight, tanX * ndc[0]), mul(camTop, tanY * ndc[1])));
    return [mul(ray, zview), zview];
};
const fastAcos = v => {
    v = clamp(v, -1, 1);
    let r = -0.156583 * Math.abs(v) + Math.PI / 2;
    r *= Math.sqrt(1 - Math.abs(v));
    return v >= 0 ? r : Math.PI - r;
};

const SLICES = 4, NSAMP = 3, RADIUS = 4.0;
const invres = [1 / W, 1 / H];

function gtao(px, py) {
    const uv = [(px + 0.5) / W, (py + 0.5) / H];
    if (sampleDepth(uv) >= 0.9999) return 1.0;
    const [cpos, cw] = fetchPos(uv);
    const viewV = mul(norm(cpos), -1);
    const [R, Rw] = fetchPos([uv[0] + invres[0], uv[1]]);
    const [Lp, Lw] = fetchPos([uv[0] - invres[0], uv[1]]);
    const [U, Uw] = fetchPos([uv[0], uv[1] + invres[1]]);
    const [D, Dw] = fetchPos([uv[0], uv[1] - invres[1]]);
    const ddx = Math.abs(Rw - cw) < Math.abs(cw - Lw) ? sub(R, cpos) : sub(cpos, Lp);
    const ddy = Math.abs(Uw - cw) < Math.abs(cw - Dw) ? sub(U, cpos) : sub(cpos, D);
    let N = cross(ddy, ddx);
    if (len(N) < 1e-12) return 1.0;
    N = norm(N);
    if (dot(N, viewV) < 0) N = mul(N, -1);

    const projScale = H / (2 * tanY);
    const screenRadius = (RADIUS * 0.5 * projScale) / cw;
    const ign2 = x => x - Math.floor(x);
    const noiseOffset = ign2(52.9829189 * ign2((px + 5.588238) * 0.06711056 + (py + 5.588238) * 0.00583715));
    const ign = x => x - Math.floor(x);
    const noiseDirection = ign(52.9829189 * ign(px * 0.06711056 + py * 0.00583715));
    const falloffMul = 2 / (RADIUS * RADIUS);
    const srm = [invres[0] / NSAMP, invres[1] / NSAMP];

    let vis = 0;
    for (let sl = 0; sl < SLICES; ++sl) {
        const phi = (sl + noiseDirection) * Math.PI / SLICES;
        const omega = [Math.cos(phi), Math.sin(phi)];
        const dirV = add(mul(camRight, omega[0]), mul(camTop, omega[1]));
        const orthoDir = sub(dirV, mul(viewV, dot(dirV, viewV)));
        const axisV = cross(dirV, viewV);
        const projN = sub(N, mul(axisV, dot(N, axisV)));
        const pl = len(projN);
        if (pl < 1e-12) continue;
        const sgnN = Math.sign(dot(orthoDir, projN)) || 1;
        const cosN = clamp(dot(projN, viewV) / pl, 0, 1);
        const nang = sgnN * fastAcos(cosN);
        const sinN2 = 2 * Math.sin(nang);
        for (let side = 0; side < 2; ++side) {
            const ss = -1 + 2 * side;
            let chc = -1;
            for (let k = 0; k < NSAMP; ++k) {
                const s = [Math.max(screenRadius * (k + noiseOffset), 2 + k) * srm[0],
                           Math.max(screenRadius * (k + noiseOffset), 2 + k) * srm[1]];
                const st = [uv[0] + ss * s[0] * omega[0], uv[1] + ss * s[1] * -omega[1]];
                const [sp] = fetchPos(st);
                const hv = sub(sp, cpos);
                const fall = clamp(dot(hv, hv) * falloffMul, 0, 1);
                const hl = len(hv);
                const Hc = hl > 1e-9 ? dot(mul(hv, 1 / hl), viewV) : -1;
                if (Hc > chc) chc = Hc * (1 - fall) + chc * fall;
            }
            const h = nang + clamp(ss * fastAcos(chc) - nang, -Math.PI / 2, Math.PI / 2);
            vis += pl * (cosN + h * sinN2 - Math.cos(2 * h - nang)) / 4;
        }
    }
    return clamp(vis / SLICES, 0, 1);
}

// Column through the wall->seam->floor + an open-floor row for baseline.
const col = Math.floor(W / 2);
console.log("center column (top=wall, bottom=near floor):");
for (let py = 4; py < H; py += 4) {
    const uv = [(col + 0.5) / W, (py + 0.5) / H];
    const [, zv] = fetchPos(uv);
    console.log(`  py=${String(py).padStart(3)} zview=${zv.toFixed(2).padStart(8)} vis=${gtao(col, py).toFixed(3)}`);
}
let flat = [];
for (let px = 8; px < W; px += 8) flat.push(gtao(px, H - 8));
console.log("near-floor row vis:", flat.map(v => v.toFixed(2)).join(" "));
