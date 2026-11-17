// Dark Riddle - Nintendo Switch port. The neighbour as an animated character
// (romfs:/char.pak, written by work/make_char.py): skinned mesh, humanoid skeleton,
// the original TeacherAnimControl clips evaluated in Mecanim muscle space.
#include "port.h"
#include <functional>

struct CBone {
    std::string name;
    int parent = -1;
    float t[3], q[4], s[3];
    Mat4 bind;
    bool axes = false;
    float pre[4], post[4], sgn[3], mn[3], mx[3];
    int dof[3] = {-1, -1, -1};
};
struct CClip {
    std::string name;
    float fps = 30;
    int frames = 0, nch = 0;
    bool loop = false;
    std::vector<float> data;
};
struct CVert { float p[3], n[3], uv[2]; uint8_t bi[4]; float bw[4]; };

static std::vector<CVert> s_verts;
static std::vector<CBone> s_bones;
static std::vector<int> s_order;          // parents before children
static std::vector<CClip> s_clips;
static std::vector<CharLevel> s_levels;
// NeighborNew's own transform scale (1.5 in every level). The skeleton is baked relative
// to NeighborNew (make_char.py), so its scale is applied here: he is 1.5x a player.
static const float NB_SCALE = 1.5f;
static int s_hips = 0;
static float s_scale = 1, s_rootXT[3], s_rootXQ[4];
static Mesh s_mesh;
static unsigned int s_skinVbo = 0, s_skinIbo = 0;   // GPU skinning buffers (uploaded on first draw)
static int s_texId = 4000;
static bool s_loaded = false;
static float s_gocT[3] = {0, 1.6f, 1}, s_gocQ[4] = {0, 1, 0, 0}, s_gocFov = 60;

static bool rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }
template <class T> static T rv(FILE* f) { T v{}; rd(f, &v, sizeof v); return v; }

bool charLoad(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[char] cannot open %s\n", path); return false; }
    char magic[4]; uint32_t ver = 0;
    if (!rd(f, magic, 4) || memcmp(magic, "DRCH", 4) != 0) { fclose(f); return false; }
    ver = rv<uint32_t>(f); (void)ver;
    // texture
    Texture tex;
    tex.w = (int)rv<uint32_t>(f); tex.h = (int)rv<uint32_t>(f);
    uint32_t tb = rv<uint32_t>(f);
    tex.data.resize(tb); rd(f, tex.data.data(), tb);
    tex.id = s_texId;
    snprintf(tex.name, sizeof tex.name, "neighbor");
    g_textures.push_back(std::move(tex));
    Material mat; mat.texId = s_texId;
    g_materials.push_back(mat);
    // mesh
    uint32_t nv = rv<uint32_t>(f), ni = rv<uint32_t>(f);
    s_verts.resize(nv);
    rd(f, s_verts.data(), nv * sizeof(CVert));
    s_mesh.idx.resize(ni);
    rd(f, s_mesh.idx.data(), ni * 4);
    s_mesh.vcount = (int)nv; s_mesh.icount = (int)ni;
    s_mesh.matId = (int)g_materials.size();
    s_mesh.verts.assign((size_t)nv * 24, 0);
    // skeleton
    uint32_t nb = rv<uint32_t>(f);
    s_hips = rv<int32_t>(f);
    s_scale = rv<float>(f);
    rd(f, s_rootXT, 12); rd(f, s_rootXQ, 16);
    s_bones.resize(nb);
    for (auto& b : s_bones) {
        char nm[32]; rd(f, nm, 32); nm[31] = 0; b.name = nm;
        b.parent = rv<int32_t>(f);
        rd(f, b.t, 12); rd(f, b.q, 16); rd(f, b.s, 12);
        rd(f, b.bind.m, 64);
        b.axes = rv<uint32_t>(f) != 0;
        rd(f, b.pre, 16); rd(f, b.post, 16); rd(f, b.sgn, 12); rd(f, b.mn, 12); rd(f, b.mx, 12);
        rd(f, b.dof, 12);
    }
    // evaluation order: parents first
    std::vector<int> state(nb, 0);
    std::function<void(int)> visit = [&](int i) {
        if (state[i]) return;
        state[i] = 1;
        if (s_bones[i].parent >= 0) visit(s_bones[i].parent);
        s_order.push_back(i);
    };
    for (uint32_t i = 0; i < nb; i++) visit((int)i);
    // clips
    uint32_t nc = rv<uint32_t>(f);
    s_clips.resize(nc);
    for (auto& c : s_clips) {
        char nm[32]; rd(f, nm, 32); nm[31] = 0; c.name = nm;
        c.fps = rv<float>(f); c.frames = (int)rv<uint32_t>(f); c.loop = rv<uint32_t>(f) != 0; c.nch = (int)rv<uint32_t>(f);
        c.data.resize((size_t)c.frames * c.nch);
        rd(f, c.data.data(), c.data.size() * 4);
    }
    // per-level NeighborAI data
    uint32_t nl = rv<uint32_t>(f);
    s_levels.resize(nl);
    for (auto& L : s_levels) {
        L.level = (int)rv<uint32_t>(f);
        uint32_t st = rv<uint32_t>(f);
        L.present = st != 0; L.active = st == 1;
        if (!L.present) continue;
        float t[3], q[4];
        rd(f, t, 12); rd(f, q, 16);
        L.pos = Vec3(t[0], t[1], t[2]);
        // facing = yaw of the transform's forward
        Mat4 r; Quat qq; qq.x = q[0]; qq.y = q[1]; qq.z = q[2]; qq.w = q[3];
        r = matFromQuat(qq);
        L.yaw = atan2f(r.m[8], r.m[10]);
        L.seeDistance = rv<float>(f); L.attackDistance = rv<float>(f);
        uint32_t nw = rv<uint32_t>(f);
        for (uint32_t k = 0; k < nw; k++) { float p[3]; rd(f, p, 12); L.waypoints.push_back(Vec3(p[0], p[1], p[2])); }
    }
    rd(f, s_gocT, 12); rd(f, s_gocQ, 16); rd(f, &s_gocFov, 4);
    fclose(f);
    s_loaded = true;
    printf("[char] %u verts, %u bones, %u clips, %u levels\n", nv, nb, nc, nl);
    return true;
}

const CharLevel* charLevel(int level) {
    for (auto& L : s_levels) if (L.level == level) return &L;
    return nullptr;
}

// ------------------------------------------------------------------ quaternions (x y z w)
static void qmul(const float* a, const float* b, float* o) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void qnorm(float* q) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-8f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= l;
}
static void qrot(const float* q, const float* v, float* o) {
    float p[4] = {v[0], v[1], v[2], 0}, c[4] = {-q[0], -q[1], -q[2], q[3]}, t[4], r[4];
    qmul(q, p, t); qmul(t, c, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}

// Mecanim axes: muscle values -> local rotation
//   angle_i = m_i * (m_i < 0 ? -min_i : max_i);  h = tan(angle / 2) * sgn
//   q = preQ * ZYRoll(h) * conj(postQ),  ZYRoll(h) = (h.x, h.y + h.x h.z, h.z - h.x h.y, 1)
static void axesToQuat(const CBone& b, const float* m, float* out) {
    float h[3];
    for (int i = 0; i < 3; i++) {
        float v = m[i];
        float a = v < 0 ? v * -b.mn[i] : v * b.mx[i];
        h[i] = tanf(0.5f * a) * b.sgn[i];
    }
    float zy[4] = {h[0], h[1] + h[0] * h[2], h[2] - h[0] * h[1], 1.f};
    qnorm(zy);
    float t[4], cp[4] = {-b.post[0], -b.post[1], -b.post[2], b.post[3]};
    qmul(b.pre, zy, t);
    qmul(t, cp, out);
    qnorm(out);
}

// ------------------------------------------------------------------ animation
static void sampleClip(int ci, float time, float* out) {
    const CClip& c = s_clips[ci];
    float len = (c.frames - 1) / c.fps;
    float t = time;
    if (c.loop && len > 0) t = fmodf(t, len); else t = std::min(t, len);
    float fr = t * c.fps;
    int f0 = std::min(c.frames - 1, (int)fr), f1 = std::min(c.frames - 1, f0 + 1);
    float a = fr - f0;
    const float* A = &c.data[(size_t)f0 * c.nch];
    const float* B = &c.data[(size_t)f1 * c.nch];
    for (int i = 0; i < c.nch; i++) out[i] = A[i] + (B[i] - A[i]) * a;
    // RootQ: keep hemisphere before the lerp
    if (A[3] * B[3] + A[4] * B[4] + A[5] * B[5] + A[6] * B[6] < 0)
        for (int i = 3; i < 7; i++) out[i] = A[i] - (B[i] + A[i]) * a;
    qnorm(out + 3);
}

// TeacherAnimControl: idle(0) walk(1) Basic_Run_03_Root(2) Dizzy(3), 0.25 s transitions
//   idle -> walk: WalkSpeed > 0.01 && < 3   idle/walk -> run: > 3
//   walk/run -> idle: < 0.01                run -> walk: < 3
//   any (but Dizzy) -> Dizzy: Dizzy;        Dizzy -> walk: !Dizzy
void charAnimUpdate(CharAnim& a, float dt, float walkSpeed, bool dizzy) {
    int next = a.state;
    switch (a.state) {
        case 0:
            if (dizzy) next = 3;
            else if (walkSpeed > 3.f) next = 2;
            else if (walkSpeed > 0.01f) next = 1;
            break;
        case 1:
            if (dizzy) next = 3;
            else if (walkSpeed > 3.f) next = 2;
            else if (walkSpeed < 0.01f) next = 0;
            break;
        case 2:
            if (dizzy) next = 3;
            else if (walkSpeed < 0.01f) next = 0;
            else if (walkSpeed < 3.f) next = 1;
            break;
        case 3:
            if (!dizzy) next = 1;
            break;
    }
    if (next != a.state && a.blend >= 1.f) {
        a.prevState = a.state; a.prevT = a.t;
        a.state = next; a.t = 0; a.blend = 0;
    }
    a.t += dt; a.prevT += dt;
    a.blend = std::min(1.f, a.blend + dt / 0.25f);
}

static Mat4 trsMat(const float* t, const float* q, const float* s) {
    Quat qq; qq.x = q[0]; qq.y = q[1]; qq.z = q[2]; qq.w = q[3];
    return matTranslate(Vec3(t[0], t[1], t[2])).mul(matFromQuat(qq)).mul(matScale(Vec3(s[0], s[1], s[2])));
}

void charDraw(const CharAnim& a, const Vec3& pos, float yaw) {
    if (!s_loaded || s_clips.empty()) return;
    // muscle-space pose: crossfade of the previous and current state's clips
    float cur[128], prv[128];
    int nch = s_clips[0].nch;
    sampleClip(std::min(a.state, (int)s_clips.size() - 1), a.t, cur);
    if (a.blend < 1.f) {
        sampleClip(std::min(a.prevState, (int)s_clips.size() - 1), a.prevT, prv);
        float w = a.blend;
        float d = prv[3] * cur[3] + prv[4] * cur[4] + prv[5] * cur[5] + prv[6] * cur[6];
        for (int i = 0; i < nch; i++) {
            float p = (i >= 3 && i < 7 && d < 0) ? -prv[i] : prv[i];
            cur[i] = p + (cur[i] - p) * w;
        }
        qnorm(cur + 3);
    }
    const float* muscles = cur + 7;

    // local transforms
    std::vector<Mat4> world(s_bones.size());
    for (int bi : s_order) {
        const CBone& b = s_bones[bi];
        float q[4] = {b.q[0], b.q[1], b.q[2], b.q[3]};
        float t[3] = {b.t[0], b.t[1], b.t[2]};
        if (bi == s_hips) {
            // body pose: RootQ relative to the rest body (m_RootX), RootT in human scale
            float rxc[4] = {-s_rootXQ[0], -s_rootXQ[1], -s_rootXQ[2], s_rootXQ[3]}, R[4], tmp[4];
            qmul(cur + 3, rxc, R);
            qmul(R, b.q, tmp); q[0] = tmp[0]; q[1] = tmp[1]; q[2] = tmp[2]; q[3] = tmp[3];
            float off[3] = {b.t[0] - s_rootXT[0], b.t[1] - s_rootXT[1], b.t[2] - s_rootXT[2]}, ro[3];
            qrot(R, off, ro);
            t[0] = cur[0] * s_scale + ro[0];
            t[1] = cur[1] * s_scale + ro[1];
            t[2] = cur[2] * s_scale + ro[2];
        } else if (b.axes && (b.dof[0] >= 0 || b.dof[1] >= 0 || b.dof[2] >= 0)) {
            float m[3];
            for (int k = 0; k < 3; k++) m[k] = b.dof[k] >= 0 ? muscles[b.dof[k]] : 0.f;
            axesToQuat(b, m, q);
        }
        Mat4 local = trsMat(t, q, b.s);
        world[bi] = b.parent >= 0 ? world[b.parent].mul(local) : local;
    }
    std::vector<Mat4> skin(s_bones.size());
    for (size_t i = 0; i < s_bones.size(); i++) skin[i] = world[i].mul(s_bones[i].bind);

    // GPU skinning: static vertex data uploaded once, 52 bone matrices per frame
    if (!s_skinVbo) {
        std::vector<float> vb(s_verts.size() * 16);
        for (size_t v = 0; v < s_verts.size(); v++) {
            const CVert& cv = s_verts[v];
            float* o = &vb[v * 16];
            o[0] = cv.p[0]; o[1] = cv.p[1]; o[2] = cv.p[2];
            o[3] = cv.n[0]; o[4] = cv.n[1]; o[5] = cv.n[2];
            o[6] = cv.uv[0]; o[7] = cv.uv[1];
            for (int k = 0; k < 4; k++) { o[8 + k] = (float)cv.bi[k]; o[12 + k] = cv.bw[k] > 0 ? cv.bw[k] : 0.f; }
        }
        s_skinVbo = gfxSkinUpload(vb.data(), (int)s_verts.size(), s_mesh.idx.data(), (int)s_mesh.idx.size(), &s_skinIbo);
    }
    std::vector<float> rows(skin.size() * 12);
    for (size_t i = 0; i < skin.size(); i++) {
        const float* M = skin[i].m;
        for (int r = 0; r < 3; r++) {
            rows[i * 12 + r * 4 + 0] = M[r]; rows[i * 12 + r * 4 + 1] = M[4 + r];
            rows[i * 12 + r * 4 + 2] = M[8 + r]; rows[i * 12 + r * 4 + 3] = M[12 + r];
        }
    }
    Mat4 model = matTranslate(pos).mul(matRotY(yaw)).mul(matScale(Vec3(NB_SCALE, NB_SCALE, NB_SCALE)));
    gfxDrawSkinned(s_skinVbo, s_skinIbo, (int)s_mesh.idx.size(), s_mesh.matId, rows.data(), (int)skin.size(), model);
}

// GameOverCamera (child of NeighborNew): world eye and forward for the catch shot
void charGameOverCamera(const Vec3& pos, float yaw, Vec3& eye, Vec3& fwd, float& fov) {
    Mat4 root = matTranslate(pos).mul(matRotY(yaw)).mul(matScale(Vec3(NB_SCALE, NB_SCALE, NB_SCALE)));
    eye = root.transformPoint(Vec3(s_gocT[0], s_gocT[1], s_gocT[2]));
    float f0[3] = {0, 0, 1}, fl[3];
    qrot(s_gocQ, f0, fl);
    Vec3 fw = root.transformPoint(Vec3(s_gocT[0] + fl[0], s_gocT[1] + fl[1], s_gocT[2] + fl[2]));
    fwd = (fw - eye).norm();
    fov = s_gocFov;
}
