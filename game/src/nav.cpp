// Dark Riddle - Nintendo Switch port. The levels' baked Unity NavMesh (romfs:/nav.pak,
// written by work/make_nav.py): polygon lookup, A* over polygons and funnel
// string pulling, standing in for NavMeshAgent pathing.
#include "port.h"
#include <queue>

struct NPoly { int n = 0; int v[6]; Vec3 c; std::vector<int> links; };
struct NLink { int a, edge, b; Vec3 p, q; };

static std::vector<Vec3> s_v;
static std::vector<NPoly> s_p;
static std::vector<NLink> s_l;
static bool s_ok = false;

bool navLoad(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[nav] cannot open %s\n", path); return false; }
    char m[4]; uint32_t ver = 0, n = 0;
    if (fread(m, 1, 4, f) != 4 || memcmp(m, "DRNV", 4) != 0) { fclose(f); return false; }
    if (fread(&ver, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
    s_v.resize(n);
    for (auto& v : s_v) { float t[3]; if (fread(t, 4, 3, f) != 3) break; v = Vec3(t[0], t[1], t[2]); }
    if (fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
    s_p.resize(n);
    for (auto& p : s_p) {
        uint8_t hdr[4]; uint32_t vs[6];
        if (fread(hdr, 1, 4, f) != 4 || fread(vs, 4, 6, f) != 6) break;
        p.n = hdr[0];
        Vec3 c;
        for (int i = 0; i < 6; i++) p.v[i] = (int)vs[i];
        for (int i = 0; i < p.n; i++) c += s_v[p.v[i]];
        p.c = c * (1.f / std::max(1, p.n));
    }
    if (fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
    s_l.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h[3]; float t[6];
        if (fread(h, 4, 3, f) != 3 || fread(t, 4, 6, f) != 6) break;
        s_l[i] = {(int)h[0], (int)h[1], (int)h[2], Vec3(t[0], t[1], t[2]), Vec3(t[3], t[4], t[5])};
        if ((int)h[0] < (int)s_p.size()) s_p[h[0]].links.push_back((int)i);
    }
    fclose(f);
    s_ok = !s_p.empty();
    printf("[nav] %zu polys, %zu links\n", s_p.size(), s_l.size());
    return s_ok;
}

bool navReady() { return s_ok; }

static float area2(const Vec3& a, const Vec3& b, const Vec3& c) {  // signed, > 0 when c is left of a->b
    return -((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z));
}

static bool inPolyXZ(const NPoly& p, const Vec3& q) {
    bool pos = false, neg = false;
    for (int i = 0; i < p.n; i++) {
        const Vec3& a = s_v[p.v[i]];
        const Vec3& b = s_v[p.v[(i + 1) % p.n]];
        float s = area2(a, b, q);
        if (s > 1e-5f) pos = true; else if (s < -1e-5f) neg = true;
        if (pos && neg) return false;
    }
    return true;
}

// floor height inside a convex polygon (fan triangles, barycentric)
static float polyHeight(const NPoly& p, const Vec3& q) {
    const Vec3& a = s_v[p.v[0]];
    for (int i = 1; i + 1 < p.n; i++) {
        const Vec3& b = s_v[p.v[i]];
        const Vec3& c = s_v[p.v[i + 1]];
        float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (fabsf(d) < 1e-9f) continue;
        float u = ((b.z - c.z) * (q.x - c.x) + (c.x - b.x) * (q.z - c.z)) / d;
        float v = ((c.z - a.z) * (q.x - c.x) + (a.x - c.x) * (q.z - c.z)) / d;
        float w = 1.f - u - v;
        if (u >= -1e-3f && v >= -1e-3f && w >= -1e-3f) return a.y * u + b.y * v + c.y * w;
    }
    return p.c.y;
}

static Vec3 closestOnSeg(const Vec3& a, const Vec3& b, const Vec3& p) {
    Vec3 ab = b - a;
    float l = ab.x * ab.x + ab.z * ab.z;
    float t = l > 1e-9f ? ((p.x - a.x) * ab.x + (p.z - a.z) * ab.z) / l : 0.f;
    t = clampf(t, 0.f, 1.f);
    return a + ab * t;
}

// polygon under/near p: containing in XZ with the floor closest below head height,
// else the nearest polygon edge point (NavMesh.SamplePosition)
int navFindPoly(const Vec3& p, Vec3* snapped) {
    int best = -1; float bestScore = 1e9f; Vec3 bestPt;
    for (size_t i = 0; i < s_p.size(); i++) {
        const NPoly& P = s_p[i];
        if (inPolyXZ(P, p)) {
            float h = polyHeight(P, p);
            float dy = p.y - h;
            float score = dy >= -0.6f ? fabsf(dy) : 10.f + fabsf(dy);   // prefer the floor below
            if (score < bestScore) { bestScore = score; best = (int)i; bestPt = Vec3(p.x, h, p.z); }
        }
    }
    // nothing containing the point at this height (a seam between tiles, or only another
    // floor above/below): the nearest polygon edge point in 3D, like SamplePosition
    if (best < 0 || bestScore >= 1.2f) {
        int eb = -1; float ed = 1e9f; Vec3 ept;
        for (size_t i = 0; i < s_p.size(); i++) {
            const NPoly& P = s_p[i];
            for (int k = 0; k < P.n; k++) {
                Vec3 c = closestOnSeg(s_v[P.v[k]], s_v[P.v[(k + 1) % P.n]], p);
                float d = (c - p).len();
                if (d < ed) { ed = d; eb = (int)i; ept = c; }
            }
        }
        if (eb >= 0 && (best < 0 || ed < 3.f)) { best = eb; bestPt = ept; }
    }
    if (snapped && best >= 0) *snapped = bestPt;
    return best;
}

// A* over polygons (portal midpoints), then the funnel algorithm
bool navPath(const Vec3& from, const Vec3& to, std::vector<Vec3>& out) {
    out.clear();
    if (!s_ok) return false;
    Vec3 s, g;
    int ps = navFindPoly(from, &s), pg = navFindPoly(to, &g);
    if (ps < 0 || pg < 0) return false;
    if (ps == pg) { out.push_back(s); out.push_back(g); return true; }
    size_t N = s_p.size();
    std::vector<float> cost(N, 1e30f);
    std::vector<int> via(N, -1);           // link index used to enter the polygon
    std::vector<Vec3> at(N);
    std::vector<char> closed(N, 0);
    typedef std::pair<float, int> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE> > open;
    cost[ps] = 0; at[ps] = s;
    open.push(QE((g - s).len(), ps));
    bool found = false;
    while (!open.empty()) {
        int cur = open.top().second; open.pop();
        if (closed[cur]) continue;
        closed[cur] = 1;
        if (cur == pg) { found = true; break; }
        for (int li : s_p[cur].links) {
            const NLink& L = s_l[li];
            if (closed[L.b]) continue;
            Vec3 mid = (L.p + L.q) * 0.5f;
            float c = cost[cur] + (mid - at[cur]).len();
            if (c < cost[L.b]) {
                cost[L.b] = c; via[L.b] = li; at[L.b] = mid;
                open.push(QE(c + (g - mid).len(), L.b));
            }
        }
    }
    if (!found) { out.push_back(s); out.push_back(navFindPoly(to, &g) >= 0 ? g : s); return false; }
    // portals from start to goal, oriented left/right of travel
    std::vector<int> chain;
    for (int c = pg; c != ps; c = s_l[via[c]].a) chain.push_back(via[c]);
    std::reverse(chain.begin(), chain.end());
    std::vector<Vec3> pl, pr;
    pl.push_back(s); pr.push_back(s);
    for (int li : chain) {
        const NLink& L = s_l[li];
        const Vec3& ca = s_p[L.a].c;
        const Vec3& cb = s_p[L.b].c;
        bool pLeft = area2(ca, cb, L.p) > area2(ca, cb, L.q);
        pl.push_back(pLeft ? L.p : L.q);
        pr.push_back(pLeft ? L.q : L.p);
    }
    pl.push_back(g); pr.push_back(g);
    // simple stupid funnel
    out.push_back(s);
    Vec3 apex = s, left = pl[0], right = pr[0];
    int ai = 0, li = 0, ri = 0;
    auto same = [](const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dz = a.z - b.z; return dx * dx + dz * dz < 1e-6f; };
    for (int i = 1; i < (int)pl.size(); i++) {
        const Vec3& L = pl[i];
        const Vec3& R = pr[i];
        // right edge tightens when the new point is left of apex->right
        if (area2(apex, right, R) >= 0.f) {
            if (same(apex, right) || area2(apex, left, R) < 0.f) { right = R; ri = i; }
            else {
                out.push_back(left); apex = left; ai = li;
                left = right = apex; li = ri = ai; i = ai; continue;
            }
        }
        if (area2(apex, left, L) <= 0.f) {
            if (same(apex, left) || area2(apex, right, L) > 0.f) { left = L; li = i; }
            else {
                out.push_back(right); apex = right; ai = ri;
                left = right = apex; li = ri = ai; i = ai; continue;
            }
        }
    }
    if (!same(out.back(), g)) out.push_back(g);
    return true;
}
