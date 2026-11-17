// Animated trees: the level trees are 2-bone skinned meshes swayed by a legacy Animation
// clip (TreeSmall_Anim, loop, played automatically - all trees in step, as in Unity).
// Data from work/make_trees.py (romfs trees.pak, format described there).
#include "port.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace {
struct TSub { unsigned int vbo = 0, ibo = 0; int ni = 0, matId = 0; };
struct TType {
    std::vector<float> verts;               // 16 floats per vertex (until uploaded)
    int nv = 0;
    std::vector<std::vector<uint32_t>> idx; // per sub (until uploaded)
    std::vector<TSub> subs;
    std::vector<int> parent;
    std::vector<Mat4> bind;
    int nf = 0; float fps = 30;
    std::vector<float> frames;              // nf * nb * 10 (t3 q4 s3)
};
struct TInst { int type; Mat4 world; };
std::vector<TType> s_types;
std::vector<std::vector<TInst>> s_levels;   // index = level number
bool s_loaded = false;
int s_texId = 4100;

bool rd(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }
template <class T> T rv(FILE* f) { T v{}; rd(f, &v, sizeof v); return v; }

Mat4 trs(const float* t, const float* q, const float* s) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 m;
    m.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; m.m[1] = (2 * (x * y + z * w)) * s[0]; m.m[2] = (2 * (x * z - y * w)) * s[0]; m.m[3] = 0;
    m.m[4] = (2 * (x * y - z * w)) * s[1]; m.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; m.m[6] = (2 * (y * z + x * w)) * s[1]; m.m[7] = 0;
    m.m[8] = (2 * (x * z + y * w)) * s[2]; m.m[9] = (2 * (y * z - x * w)) * s[2]; m.m[10] = (1 - 2 * (x * x + y * y)) * s[2]; m.m[11] = 0;
    m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2]; m.m[15] = 1;
    return m;
}
}  // namespace

bool treesLoad(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[trees] cannot open %s\n", path); return false; }
    char magic[4];
    if (!rd(f, magic, 4) || memcmp(magic, "DRTR", 4) != 0) { fclose(f); return false; }
    rv<uint32_t>(f);
    uint32_t nt = rv<uint32_t>(f);
    s_types.resize(nt);
    for (auto& t : s_types) {
        t.nv = (int)rv<uint32_t>(f);
        t.verts.resize((size_t)t.nv * 16); rd(f, t.verts.data(), t.verts.size() * 4);
        uint32_t ns = rv<uint32_t>(f);
        t.idx.resize(ns); t.subs.resize(ns);
        for (uint32_t s = 0; s < ns; s++) {
            uint32_t ni = rv<uint32_t>(f);
            t.idx[s].resize(ni); rd(f, t.idx[s].data(), ni * 4);
            Texture tex;
            tex.w = (int)rv<uint32_t>(f); tex.h = (int)rv<uint32_t>(f);
            tex.data.resize((size_t)tex.w * tex.h * 4); rd(f, tex.data.data(), tex.data.size());
            tex.id = s_texId++;
            snprintf(tex.name, sizeof tex.name, "tree%u_%u", (unsigned)(&t - s_types.data()), s);
            float c[5]; rd(f, c, sizeof c);
            Material mat; mat.texId = tex.id;
            mat.r = c[0]; mat.g = c[1]; mat.b = c[2]; mat.a = c[3];
            if (c[4] > 0) { mat.cutout = 1; mat.cutoff = c[4]; }
            g_textures.push_back(std::move(tex));
            g_materials.push_back(mat);
            t.subs[s].ni = (int)ni; t.subs[s].matId = (int)g_materials.size();
        }
        uint32_t nb = rv<uint32_t>(f);
        t.parent.resize(nb); t.bind.resize(nb);
        for (uint32_t b = 0; b < nb; b++) { t.parent[b] = rv<int32_t>(f); rd(f, t.bind[b].m, 64); }
        t.nf = (int)rv<uint32_t>(f); t.fps = rv<float>(f);
        t.frames.resize((size_t)t.nf * nb * 10); rd(f, t.frames.data(), t.frames.size() * 4);
    }
    uint32_t nl = rv<uint32_t>(f);
    for (uint32_t l = 0; l < nl; l++) {
        uint32_t lv = rv<uint32_t>(f), ni = rv<uint32_t>(f);
        if (lv >= s_levels.size()) s_levels.resize(lv + 1);
        for (uint32_t i = 0; i < ni; i++) {
            TInst in; in.type = (int)rv<uint32_t>(f); rd(f, in.world.m, 64);
            if (in.type >= 0 && in.type < (int)s_types.size()) s_levels[lv].push_back(in);
        }
    }
    bool ok = true;
    fclose(f);
    s_loaded = ok;
    printf("[trees] %u types, %u levels\n", nt, nl);
    return ok;
}

void treesDraw(int level, float time) {
    if (!s_loaded || level < 0 || level >= (int)s_levels.size()) return;
    for (const TInst& in : s_levels[level]) {
        TType& t = s_types[in.type];
        if (t.subs.empty() || t.nf <= 0) continue;
        if (!t.subs[0].vbo) {
            for (size_t s = 0; s < t.subs.size(); s++) {
                t.subs[s].vbo = gfxSkinUpload(t.verts.data(), t.nv, t.idx[s].data(), (int)t.idx[s].size(), &t.subs[s].ibo);
            }
        }
        // sample the looping clip (linear between 30 fps frames, quaternion nlerp)
        int nb = (int)t.parent.size();
        float len = (t.nf - 1) / t.fps;
        float ph = len > 0 ? fmodf(time, len) * t.fps : 0.f;
        int f0 = (int)ph; if (f0 >= t.nf - 1) f0 = t.nf - 2; if (f0 < 0) f0 = 0;
        int f1 = t.nf > 1 ? f0 + 1 : 0;
        float a = ph - f0; if (a < 0) a = 0; if (a > 1) a = 1;
        std::vector<Mat4> world(nb);
        std::vector<float> rows((size_t)nb * 12);
        for (int b = 0; b < nb; b++) {
            const float* A = &t.frames[((size_t)f0 * nb + b) * 10];
            const float* B = &t.frames[((size_t)f1 * nb + b) * 10];
            float v[10];
            float d = A[3] * B[3] + A[4] * B[4] + A[5] * B[5] + A[6] * B[6];
            for (int k = 0; k < 10; k++) {
                float bk = (k >= 3 && k < 7 && d < 0) ? -B[k] : B[k];
                v[k] = A[k] + (bk - A[k]) * a;
            }
            float ql = sqrtf(v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
            if (ql > 1e-6f) for (int k = 3; k < 7; k++) v[k] /= ql;
            Mat4 local = trs(v, v + 3, v + 7);
            // bones are children of the animated root (parent -1 = the tree's own transform)
            world[b] = (t.parent[b] >= 0 ? world[t.parent[b]] : in.world).mul(local);
            Mat4 sk = world[b].mul(t.bind[b]);
            for (int r = 0; r < 3; r++) {
                rows[b * 12 + r * 4 + 0] = sk.m[r]; rows[b * 12 + r * 4 + 1] = sk.m[4 + r];
                rows[b * 12 + r * 4 + 2] = sk.m[8 + r]; rows[b * 12 + r * 4 + 3] = sk.m[12 + r];
            }
        }
        Mat4 ident;
        g_skinFlatLight = 1.0f;   // trees: unshaded like the baked level (no neighbour key light)
        for (const TSub& s : t.subs) gfxDrawSkinned(s.vbo, s.ibo, s.ni, s.matId, rows.data(), nb, ident);
        g_skinFlatLight = 0.0f;
    }
}
