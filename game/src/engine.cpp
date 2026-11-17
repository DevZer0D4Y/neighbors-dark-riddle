// Dark Riddle - Nintendo Switch port. Renderer, pak loader, audio mixer.
#include "port.h"
#include <SDL2/SDL.h>
#include "gl_port.h"
#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#endif

std::vector<Texture> g_textures;
std::vector<Material> g_materials;
std::vector<Mesh> g_meshes;
std::vector<Scene> g_scenes;
std::vector<AudioClip> g_audio;
std::vector<Glyph> g_glyphs;
int g_fontTex = 0;
float g_fontPixelSize = 26.f;
Vec3 g_switchLight;

// ------------------------------------------------------------------ matrices
Mat4 matTranslate(const Vec3& t) { Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r; }
Mat4 matScale(const Vec3& s) { Mat4 r; r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; return r; }
Mat4 matRotY(float a) {
    Mat4 r; float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}
Mat4 matFromQuat(const Quat& q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float n = sqrtf(x * x + y * y + z * z + w * w); if (n < 1e-6f) { x = y = z = 0; w = 1; n = 1; }
    x /= n; y /= n; z /= n; w /= n;
    Mat4 r;
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + z * w); r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w); r.m[5] = 1 - 2 * (x * x + z * z); r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w); r.m[9] = 2 * (y * z - x * w); r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}
Mat4 matPerspective(float fovy, float aspect, float n, float f) {
    Mat4 r; float t = 1.f / tanf(fovy * 0.5f * 3.14159265f / 180.f);
    r.m[0] = t / aspect; r.m[5] = t;
    r.m[10] = (f + n) / (n - f); r.m[11] = -1.f;
    r.m[14] = (2 * f * n) / (n - f); r.m[15] = 0;
    return r;
}
Mat4 matOrtho(float l, float r, float b, float t, float n, float f) {
    Mat4 m;
    m.m[0] = 2.f / (r - l); m.m[5] = 2.f / (t - b); m.m[10] = -2.f / (f - n);
    m.m[12] = -(r + l) / (r - l); m.m[13] = -(t + b) / (t - b); m.m[14] = -(f + n) / (f - n); m.m[15] = 1.f;
    return m;
}
// Left-handed look-at: the level data is in Unity's left-handed space (+X is to the
// right when looking down +Z), so screen-right must map to cross(up, forward).
Mat4 matLookAt(const Vec3& eye, const Vec3& ctr, const Vec3& up) {
    Vec3 f = (ctr - eye).norm();
    Vec3 s = cross(up, f).norm();
    Vec3 u = cross(f, s);
    Mat4 r;
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye);
    return r;
}

// ------------------------------------------------------------------ pak
struct Reader {
    const uint8_t* p; size_t n, i = 0;
    Reader(const uint8_t* d, size_t s) : p(d), n(s) {}
    bool ok(size_t k) const { return i + k <= n; }
    uint32_t u32() { if (!ok(4)) { i = n; return 0; } uint32_t v; memcpy(&v, p + i, 4); i += 4; return v; }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { float v; if (!ok(4)) { i = n; return 0; } memcpy(&v, p + i, 4); i += 4; return v; }
    uint16_t u16() { if (!ok(2)) { i = n; return 0; } uint16_t v; memcpy(&v, p + i, 2); i += 2; return v; }
    uint8_t u8() { if (!ok(1)) { i = n; return 0; } return p[i++]; }
    Vec3 vec3() { float a = f32(), b = f32(), c = f32(); return Vec3(a, b, c); }
    Quat quat() { Quat q; q.x = f32(); q.y = f32(); q.z = f32(); q.w = f32(); return q; }
    void name(char* out, int n) { if (!ok(n)) { i = n; out[0] = 0; return; } memcpy(out, p + i, n); i += n; out[n - 1] = 0; }
};

static uint8_t* readFile(const char* path, size_t* outSize) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return nullptr; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *outSize = rd;
    return buf;
}

bool engineLoadPak(const char* path) {
    size_t size = 0;
    uint8_t* data = readFile(path, &size);
    if (!data) { printf("[pak] cannot open %s\n", path); return false; }
    Reader r(data, size);
    char magic[4]; memcpy(magic, data, 4); r.i = 4;
    if (memcmp(magic, "DRPK", 4) != 0) { printf("[pak] bad magic\n"); free(data); return false; }
    uint32_t ver = r.u32();
    uint32_t nTex = r.u32(), nMat = r.u32(), nMesh = r.u32(), nScene = r.u32(), nAud = r.u32();
    g_fontTex = (int)r.u32();
    printf("[pak] v%u tex=%u mat=%u mesh=%u scenes=%u audio=%u\n", ver, nTex, nMat, nMesh, nScene, nAud);

    g_textures.resize(nTex);
    for (uint32_t i = 0; i < nTex; i++) {
        Texture& t = g_textures[i];
        t.id = (int)r.u32(); t.w = r.u16(); t.h = r.u16(); t.fmt = r.u8(); r.u8();
        uint32_t sz = r.u32();
        r.name(t.name, 40);
        if (!r.ok(sz)) { printf("[pak] truncated texture\n"); break; }
        t.data.assign(r.p + r.i, r.p + r.i + sz); r.i += sz;
    }
    g_materials.resize(nMat);
    for (uint32_t i = 0; i < nMat; i++) {
        Material& m = g_materials[i];
        m.texId = (int)r.u32();
        m.r = r.f32(); m.g = r.f32(); m.b = r.f32(); m.a = r.f32();
        m.tx = r.f32(); m.ty = r.f32(); m.ox = r.f32(); m.oy = r.f32(); m.cutout = (int)r.u32();
        if (ver >= 4) m.cutoff = r.f32();
    }
    g_meshes.resize(nMesh);
    for (uint32_t i = 0; i < nMesh; i++) {
        Mesh& m = g_meshes[i];
        m.id = (int)r.u32();
        uint32_t sz = r.u32();
        if (!r.ok(sz)) { printf("[pak] truncated mesh\n"); break; }
        Reader mr(r.p + r.i, sz);
        m.matId = (int)mr.u32(); m.vcount = (int)mr.u32(); m.icount = (int)mr.u32();
        size_t vbytes = (size_t)m.vcount * 24;
        if (mr.ok(vbytes)) { m.verts.assign(mr.p + mr.i, mr.p + mr.i + vbytes); mr.i += vbytes; }
        size_t ibytes = (size_t)m.icount * 4;
        if (mr.ok(ibytes)) {
            m.idx.resize(m.icount);
            memcpy(m.idx.data(), mr.p + mr.i, ibytes);
            mr.i += ibytes;
        }
        r.i += sz;
    }
    g_scenes.resize(nScene);
    for (uint32_t i = 0; i < nScene; i++) {
        Scene& s = g_scenes[i];
        s.level = (int)r.u32();
        s.spawn = r.vec3(); s.yaw = r.f32();
        s.ambient = r.vec3(); s.fog = r.vec3();
        uint32_t nm = r.u32();
        for (uint32_t k = 0; k < nm; k++) s.meshIds.push_back((int)r.u32());
        uint32_t nc = r.u32();
        s.colliders.resize(nc);
        for (uint32_t k = 0; k < nc; k++) { s.colliders[k].mn = r.vec3(); s.colliders[k].mx = r.vec3(); }
        uint32_t np = r.u32();
        s.props.resize(np);
        for (uint32_t k = 0; k < np; k++) {
            Prop& p = s.props[k];
            p.type = (int)r.u32();
            r.name(p.name, 40);
            p.pos = r.vec3(); p.rot = r.quat(); p.scale = r.vec3();
            p.meshId = (int)r.u32();
            p.half = r.vec3(); p.center = r.vec3();
            p.restRot.x = r.f32(); p.restRot.y = r.f32(); p.restRot.z = r.f32(); p.restRot.w = r.f32();
            { uint32_t par = r.u32(); p.parent = (p.type == 13 && par != 0xffffffffu) ? (int)par : -1; }
            if (p.restRot.x == 0 && p.restRot.y == 0 && p.restRot.z == 0 && p.restRot.w == 0) p.restRot.w = 1;
        }
        uint32_t nmk = r.u32();
        s.markers.resize(nmk);
        for (uint32_t k = 0; k < nmk; k++) {
            Marker& m = s.markers[k];
            m.type = (int)r.u32(); m.pos = r.vec3(); m.radius = r.f32(); r.name(m.name, 40);
            m.half = r.vec3();
        }
        uint32_t nl = r.u32();
        s.lights.resize(nl);
        for (uint32_t k = 0; k < nl; k++) {
            LightSrc& L = s.lights[k];
            L.pos = r.vec3(); L.dir = r.vec3(); L.color = r.vec3();
            L.intensity = r.f32(); L.range = r.f32(); L.type = (int)r.u32();
        }
    }
    for (uint32_t i = 0; i < nAud; i++) {
        AudioClip c;
        char nm[32]; r.name(nm, 32);
        c.name = nm;
        c.ch = (int)r.u32(); c.freq = (int)r.u32(); c.frames = (int)r.u32();
        size_t bytes = (size_t)c.frames * c.ch * 2;
        if (!r.ok(bytes)) { printf("[pak] truncated audio\n"); break; }
        c.pcm.resize((size_t)c.frames * c.ch);
        memcpy(c.pcm.data(), r.p + r.i, bytes);
        r.i += bytes;
        g_audio.push_back(std::move(c));
    }
    g_fontTex = (int)r.u32();
    uint32_t ng = r.u32();
    g_glyphs.resize(ng);
    for (uint32_t i = 0; i < ng; i++) {
        Glyph& g = g_glyphs[i];
        g.code = r.u32(); g.x = (int)r.u32(); g.y = (int)r.u32();
        g.w = (int)r.u32(); g.h = (int)r.u32();
        g.bx = r.f32(); g.by = r.f32(); g.base = r.f32();
    }
    free(data);
    printf("[pak] loaded: %zu textures, %zu meshes, %zu audio clips\n", g_textures.size(), g_meshes.size(), g_audio.size());
    return true;
}

Texture* findTexture(const char* name) {
    for (auto& t : g_textures) if (strcmp(t.name, name) == 0) return &t;
    return nullptr;
}

static std::vector<UIElement> s_ui;
const std::vector<UIElement>& uiElements() { return s_ui; }

// romfs:/ui.pak - the Menu scene's own sprites and layout, written by work/make_ui.py
bool uiLoadPak(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("[ui] cannot open %s\n", path); return false; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)sz);
    if (fread(buf.data(), 1, buf.size(), fp) != buf.size()) { fclose(fp); return false; }
    fclose(fp);
    if (buf.size() < 16 || memcmp(buf.data(), "DRUI", 4) != 0) { printf("[ui] bad magic\n"); return false; }
    size_t p = 8;    // magic(4) + version(4), then counts
    uint32_t nTex = 0, nElem = 0;
    memcpy(&nTex, &buf[p], 4); p += 4;
    memcpy(&nElem, &buf[p], 4); p += 4;
    for (uint32_t i = 0; i < nTex; i++) {
        uint32_t w = 0, h = 0, bytes = 0;
        if (p + 12 + 32 > buf.size()) break;
        memcpy(&w, &buf[p], 4); p += 4;
        memcpy(&h, &buf[p], 4); p += 4;
        memcpy(&bytes, &buf[p], 4); p += 4;
        char name[32] = {0};
        memcpy(name, &buf[p], 32); p += 32;
        if (p + bytes > buf.size()) break;
        Texture t;
        t.id = 2000 + (int)i;
        t.w = (int)w; t.h = (int)h;
        snprintf(t.name, sizeof t.name, "%s", name);
        t.data.assign(&buf[p], &buf[p] + bytes);
        p += bytes;
        g_textures.push_back(std::move(t));
    }
    for (uint32_t i = 0; i < nElem; i++) {
        if (p + 4 + 4 + 16 + 4 + 24 > buf.size()) break;
        UIElement e;
        memcpy(&e.panel, &buf[p], 4); p += 4;
        uint32_t sid = 0;
        memcpy(&sid, &buf[p], 4); p += 4;
        memcpy(&e.cx, &buf[p], 4); p += 4;
        memcpy(&e.cy, &buf[p], 4); p += 4;
        memcpy(&e.w, &buf[p], 4); p += 4;
        memcpy(&e.h, &buf[p], 4); p += 4;
        memcpy(&e.id, &buf[p], 4); p += 4;
        memcpy(e.label, &buf[p], 24); p += 24;
        if (sid > 0 && sid <= nTex) snprintf(e.tex, sizeof e.tex, "%s", g_textures[g_textures.size() - nTex + (sid - 1)].name);
        s_ui.push_back(e);
    }
    printf("[ui] %u sprites, %u elements\n", nTex, nElem);
    return true;
}

Texture* getTexture(int id) {
    for (auto& t : g_textures) if (t.id == id) return &t;
    return nullptr;
}
Mesh* getMesh(int id) {
    if (id <= 0) return nullptr;
    for (auto& m : g_meshes) if (m.id == id) return &m;
    return nullptr;
}
Material* getMaterial(int id) {
    if (id <= 0 || id > (int)g_materials.size()) return nullptr;
    return &g_materials[id - 1];
}
AudioClip* getAudio(const char* name) {
    for (auto& c : g_audio) if (c.name == name) return &c;
    // the pak stores 31 characters of a clip name
    for (auto& c : g_audio) if (strncmp(c.name.c_str(), name, 31) == 0) return &c;
    return nullptr;
}

// ------------------------------------------------------------------ gl
static GLuint s_prog = 0, s_vbo2d = 0;
static GLuint s_white = 0;   // 1x1 white: untextured rects sample this (texture 0 reads black)
static GLint uMVP = -1, uTex = -1, uTint = -1, uAlpha = -1, uFogCol = -1, uFogDen = -1, uMode = -1;
static GLint uModel = -1, uCamPos = -1, uST = -1, uColScale = -1, uMask = -1, uMaskOn = -1, uSwitch = -1;
static Vec3 s_camPos;
static Mat4 s_viewProj;
static int s_w = 1280, s_h = 720;
static int s_winW = 1280, s_winH = 720;   // the window's framebuffer (viewport after offscreen passes)
static std::vector<float> s_quad;   // 2D vertex buffer (x,y,u,v,r,g,b,a)

static const char* VS =
    "#ifdef GL_ES\n"
    "precision highp float;\n"
    "#endif\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uModel;\n"
    "uniform vec3 uCamPos;\n"
    "uniform vec3 uFogCol;\n"
    "uniform float uFogDen;\n"
    "uniform vec4 uST;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "varying float vFog;\n"
    "void main(){\n"
    // material tiling/offset, then Unity's bottom-up V to GL's top-down texture rows
    "  vec2 t = aUV * uST.xy + uST.zw;\n"
    "  vUV = vec2(t.x, 1.0 - t.y); vCol=aCol;\n"
    "  vec4 wp = uMVP * vec4(aPos,1.0);\n"
    "  gl_Position = wp;\n"
    "  vec3 wpos = (uModel * vec4(aPos,1.0)).xyz;\n"
    "  vFog = uFogDen > 0.0 ? clamp(distance(wpos, uCamPos) * uFogDen, 0.0, 0.85) : 0.0;\n"
    "}\n";

static const char* FS =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uTint;\n"
    "uniform float uAlpha;\n"
    "uniform vec3 uFogCol;\n"
    "uniform float uFogDen;\n"
    "uniform float uColScale;\n"
    "uniform vec3 uSwitch;\n"
    "uniform sampler2D uMask;\n"
    "uniform float uMaskOn;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "varying float vFog;\n"
    "void main(){\n"
    // world meshes store baked light at half scale (0..2 range); 2D/sky use 1.0
    // their alpha holds 1 - N.L of the switchable sun (houseLights), added back while it is on
    "  bool world = uColScale > 1.5;\n"
    "  vec3 lit = world ? min(vCol.rgb * uColScale + uSwitch * (1.0 - vCol.a), vec3(2.0)) : vCol.rgb;\n"
    "  vec4 t = texture2D(uTex, vUV) * uTint * vec4(lit, world ? 1.0 : vCol.a);\n"
    "  if (uMaskOn > 0.5) t.a *= texture2D(uMask, vUV).a;\n"
    "  if (uAlpha > 0.0 && t.a < uAlpha) discard;\n"
    "  vec3 c = mix(t.rgb, uFogCol, vFog);\n"
    "  gl_FragColor = vec4(c, t.a);\n"
    "}\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); printf("[gl] shader err: %s\n", log); }
    return s;
}

bool gfxInit(int w, int h) {
    s_w = w; s_h = h; s_winW = w; s_winH = h;
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "aPos");
    glBindAttribLocation(s_prog, 1, "aUV");
    glBindAttribLocation(s_prog, 2, "aCol");
    glLinkProgram(s_prog);
    GLint ok = 0; glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(s_prog, sizeof(log), nullptr, log); printf("[gl] link err: %s\n", log); }
    uMVP = glGetUniformLocation(s_prog, "uMVP");
    uTex = glGetUniformLocation(s_prog, "uTex");
    uTint = glGetUniformLocation(s_prog, "uTint");
    uAlpha = glGetUniformLocation(s_prog, "uAlpha");
    uFogCol = glGetUniformLocation(s_prog, "uFogCol");
    uFogDen = glGetUniformLocation(s_prog, "uFogDen");
    uModel = glGetUniformLocation(s_prog, "uModel");
    uCamPos = glGetUniformLocation(s_prog, "uCamPos");
    uST = glGetUniformLocation(s_prog, "uST");
    uColScale = glGetUniformLocation(s_prog, "uColScale");
    uMask = glGetUniformLocation(s_prog, "uMask");
    uMaskOn = glGetUniformLocation(s_prog, "uMaskOn");
    uSwitch = glGetUniformLocation(s_prog, "uSwitch");
    glUseProgram(s_prog);
    glUniform1i(uMask, 1);
    glUniform1f(uMaskOn, 0.f);
    glGenBuffers(1, &s_vbo2d);
    {
        const unsigned char px[4] = {255, 255, 255, 255};
        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.02f, 0.02f, 0.03f, 1.f);
    printf("[gl] renderer %s\n", (const char*)glGetString(GL_RENDERER));
    return true;
}

static bool isPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

static void uploadTexture(Texture* t) {
    if (!t || t->gl) return;
    // world textures tile (Unity default wrap = repeat) and get mipmaps; UI sprites,
    // the font atlas and skybox faces are clamped and drawn 1:1
    bool ui = t->id >= 2000 || t->id == g_fontTex || strncmp(t->name, "sky_", 4) == 0;
    bool mips = !ui && isPow2(t->w) && isPow2(t->h);
    glGenTextures(1, &t->gl);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLint wrap = (ui || !mips) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->data.data());
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    // the GL copy is all we need from here on
    std::vector<uint8_t>().swap(t->data);
}

static void uploadMesh(Mesh* m) {
    if (!m || m->vbo) return;
    glGenBuffers(1, &m->vbo);
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, m->verts.size(), m->verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m->idx.size() * 4, m->idx.data(), GL_STATIC_DRAW);
}

static void setTexMatrix(Material* mat) {
    // baked into uv at draw time instead (keeps shader small)
    (void)mat;
}

void gfxBeginFrame(const Vec3& ambient, const Vec3& fog, float fogDensity) {
    glClearColor(fog.x, fog.y, fog.z, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(s_prog);
    glUniform3f(uFogCol, fog.x, fog.y, fog.z);
    glUniform1f(uFogDen, fogDensity);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uTex, 0);
    (void)ambient;
}

void gfxSetCamera(const Mat4& vp) { s_viewProj = vp; }
void gfxSetCameraPos(const Vec3& p) { s_camPos = p; }

void gfxDrawMesh(Mesh* m, const Mat4& model, const Color& tint, float alphaTest, bool twoSided) {
    if (!m) return;
    uploadMesh(m);
    if (m->dirty) {
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
        glBufferData(GL_ARRAY_BUFFER, m->verts.size(), m->verts.data(), GL_STREAM_DRAW);
        m->dirty = false;
    }
    Material* mat = getMaterial(m->matId);
    // texId 0 = no _MainTex: the material colour alone (white texture)
    Texture* tex = (mat && mat->texId > 0) ? getTexture(mat->texId) : nullptr;
    if (tex) uploadTexture(tex);

    Mat4 mvp = s_viewProj.mul(model);
    glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(uModel, 1, GL_FALSE, model.m);
    glUniform3f(uCamPos, s_camPos.x, s_camPos.y, s_camPos.z);
    // Standard shader: albedo = _MainTex * _Color
    if (mat) glUniform4f(uTint, tint.r * mat->r, tint.g * mat->g, tint.b * mat->b, tint.a * mat->a);
    else glUniform4f(uTint, tint.r, tint.g, tint.b, tint.a);
    glUniform1f(uAlpha, alphaTest);
    glUniform1f(uColScale, 2.f);
    glUniform3f(uSwitch, g_switchLight.x, g_switchLight.y, g_switchLight.z);
    if (mat) glUniform4f(uST, mat->tx, mat->ty, mat->ox, mat->oy);
    else glUniform4f(uST, 1, 1, 0, 0);
    glBindTexture(GL_TEXTURE_2D, tex ? tex->gl : s_white);
    // Standard-shader transparent surfaces cull back faces: a glass pane is a thin box,
    // drawing both of its sides doubles the opacity. Unity's front faces are clockwise.
    if (mat && mat->cutout == 2) { glEnable(GL_CULL_FACE); glFrontFace(GL_CW); glCullFace(GL_BACK); }
    else glDisable(GL_CULL_FACE);
    (void)twoSided;
    // only Unity "Transparent" materials blend; everything else is opaque or cutout
    if (mat && mat->cutout == 2) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
    else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }

    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, (void*)12);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 24, (void*)20);
    glDrawElements(GL_TRIANGLES, m->icount, GL_UNSIGNED_INT, 0);
    (void)setTexMatrix;
}

// ------------------------------------------------------------------ GPU skinning
// The neighbour: 4 bone influences per vertex, bone matrices as 3 rows each (Unity's
// SkinnedMeshRenderer does this on the GPU too). Lighting matches the old CPU path.
static const char* SKIN_VS =
    "#ifdef GL_ES\n"
    "precision highp float;\n"
    "#endif\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec3 aNrm;\n"
    "attribute vec4 aBi;\n"
    "attribute vec4 aBw;\n"
    "uniform mat4 uMVP;\n"
    "uniform vec4 uST;\n"
    "uniform vec4 uBones[192];\n"
    "uniform float uFlat;\n"
    "varying vec2 vUV;\n"
    "varying float vLit;\n"
    "vec3 xf(float fi, vec4 v) { int i = int(fi + 0.5) * 3;\n"
    "  return vec3(dot(uBones[i], v), dot(uBones[i + 1], v), dot(uBones[i + 2], v)); }\n"
    "void main(){\n"
    "  vec4 p = vec4(aPos, 1.0), n = vec4(aNrm, 0.0);\n"
    "  vec3 sp = xf(aBi.x, p) * aBw.x + xf(aBi.y, p) * aBw.y + xf(aBi.z, p) * aBw.z + xf(aBi.w, p) * aBw.w;\n"
    "  vec3 sn = xf(aBi.x, n) * aBw.x + xf(aBi.y, n) * aBw.y + xf(aBi.z, n) * aBw.z + xf(aBi.w, n) * aBw.w;\n"
    "  gl_Position = uMVP * vec4(sp, 1.0);\n"
    "  vec2 t = aUV * uST.xy + uST.zw;\n"
    "  vUV = vec2(t.x, 1.0 - t.y);\n"
    "  float l = length(sn);\n"
    "  float ndl = l > 1e-6 ? dot(sn / l, vec3(0.35, 0.82, -0.45)) : 0.0;\n"
    "  vLit = uFlat > 0.0 ? uFlat : 0.55 + 0.75 * max(ndl, 0.0);\n"
    "}\n";
static const char* SKIN_FS =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uTint;\n"
    "uniform float uCut;\n"
    "varying vec2 vUV;\n"
    "varying float vLit;\n"
    "void main(){\n"
    "  vec4 t = texture2D(uTex, vUV) * uTint;\n"
    "  if (t.a < uCut) discard;\n"
    "  gl_FragColor = vec4(t.rgb * min(vLit, 2.0), t.a);\n"
    "}\n";
static GLuint s_skinProg = 0;
static GLint sk_MVP = -1, sk_ST = -1, sk_Bones = -1, sk_Tex = -1, sk_Tint = -1, sk_Cut = -1, sk_Flat = -1;
float g_skinFlatLight = 0.0f;

unsigned int gfxSkinUpload(const float* verts16, int nv, const uint32_t* idx, int ni, unsigned int* iboOut) {
    if (!s_skinProg) {
        GLuint vs = compile(GL_VERTEX_SHADER, SKIN_VS), fs = compile(GL_FRAGMENT_SHADER, SKIN_FS);
        s_skinProg = glCreateProgram();
        glAttachShader(s_skinProg, vs); glAttachShader(s_skinProg, fs);
        glBindAttribLocation(s_skinProg, 0, "aPos");
        glBindAttribLocation(s_skinProg, 1, "aUV");
        glBindAttribLocation(s_skinProg, 2, "aNrm");
        glBindAttribLocation(s_skinProg, 3, "aBi");
        glBindAttribLocation(s_skinProg, 4, "aBw");
        glLinkProgram(s_skinProg);
        GLint ok = 0; glGetProgramiv(s_skinProg, GL_LINK_STATUS, &ok);
        if (!ok) { char log[1024]; glGetProgramInfoLog(s_skinProg, sizeof(log), nullptr, log); printf("[gl] skin link err: %s\n", log); }
        sk_MVP = glGetUniformLocation(s_skinProg, "uMVP");
        sk_ST = glGetUniformLocation(s_skinProg, "uST");
        sk_Bones = glGetUniformLocation(s_skinProg, "uBones");
        sk_Tex = glGetUniformLocation(s_skinProg, "uTex");
        sk_Tint = glGetUniformLocation(s_skinProg, "uTint");
        sk_Cut = glGetUniformLocation(s_skinProg, "uCut");
        sk_Flat = glGetUniformLocation(s_skinProg, "uFlat");
    }
    GLuint vbo = 0, ibo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)nv * 16 * 4, verts16, GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)ni * 4, idx, GL_STATIC_DRAW);
    *iboOut = ibo;
    return vbo;
}

void gfxDrawSkinned(unsigned int vbo, unsigned int ibo, int ni, int matId, const float* boneRows, int nb, const Mat4& model) {
    if (!vbo || !s_skinProg) return;
    Material* mat = getMaterial(matId);
    Texture* tex = (mat && mat->texId > 0) ? getTexture(mat->texId) : nullptr;
    if (tex) uploadTexture(tex);
    glUseProgram(s_skinProg);
    Mat4 mvp = s_viewProj.mul(model);
    glUniformMatrix4fv(sk_MVP, 1, GL_FALSE, mvp.m);
    if (mat) glUniform4f(sk_ST, mat->tx, mat->ty, mat->ox, mat->oy); else glUniform4f(sk_ST, 1, 1, 0, 0);
    glUniform4fv(sk_Bones, std::min(nb, 64) * 3, boneRows);
    glUniform1i(sk_Tex, 0);
    if (mat) glUniform4f(sk_Tint, mat->r, mat->g, mat->b, mat->a); else glUniform4f(sk_Tint, 1, 1, 1, 1);
    glUniform1f(sk_Cut, (mat && mat->cutout == 1) ? mat->cutoff : -1.0f);
    glUniform1f(sk_Flat, g_skinFlatLight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex ? tex->gl : s_white);
    glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDepthMask(GL_TRUE);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    for (int a = 0; a < 5; a++) glEnableVertexAttribArray(a);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 64, (void*)0);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 64, (void*)12);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 64, (void*)24);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 64, (void*)32);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 64, (void*)48);
    glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_INT, 0);
    glDisableVertexAttribArray(3); glDisableVertexAttribArray(4);
    glUseProgram(s_prog);   // back to the main program (its uniforms are untouched)
}

void gfxEnd3D() {
    glDisable(GL_CULL_FACE);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDepthMask(GL_TRUE);
}

// Unity Skybox/6 Sided: Front=+Z, Back=-Z, Left=+X, Right=-X, Up=+Y, Down=-Y,
// each face seen from inside. Drawn first with depth writes off, centred on the eye.
void gfxDrawSkybox(const Mat4& viewProjNoTranslate) {
    static const char* names[6] = {"sky_front", "sky_back", "sky_left", "sky_right", "sky_up", "sky_down"};
    // centre, right, up for each face as seen by a viewer at the origin
    static const float F[6][9] = {
        { 0, 0, 1,   1, 0, 0,   0, 1, 0},
        { 0, 0,-1,  -1, 0, 0,   0, 1, 0},
        { 1, 0, 0,   0, 0,-1,   0, 1, 0},
        {-1, 0, 0,   0, 0, 1,   0, 1, 0},
        { 0, 1, 0,   1, 0, 0,   0, 0,-1},
        { 0,-1, 0,   1, 0, 0,   0, 0, 1},
    };
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glUseProgram(s_prog);
    Mat4 id;
    glUniformMatrix4fv(uMVP, 1, GL_FALSE, viewProjNoTranslate.m);
    glUniformMatrix4fv(uModel, 1, GL_FALSE, id.m);
    glUniform1f(uFogDen, 0.f);
    glUniform1f(uAlpha, 0.f);
    glUniform4f(uTint, 1, 1, 1, 1);
    glUniform4f(uST, 1, -1, 0, 1);   // image-space UVs (cancels the Unity V flip)
    glUniform1f(uColScale, 1.f);
    const float S = 10.f;
    for (int f = 0; f < 6; f++) {
        Texture* t = findTexture(names[f]);
        if (!t) continue;
        uploadTexture(t);
        const float* c = F[f];
        float corner[4][5];
        const float su[4] = {-1, 1, 1, -1}, sv[4] = {1, 1, -1, -1};   // TL, TR, BR, BL
        // half-texel inset hides the seams between faces
        const float e = 0.5f / (float)t->w;
        for (int k = 0; k < 4; k++) {
            for (int a = 0; a < 3; a++) corner[k][a] = (c[a] + c[3 + a] * su[k] + c[6 + a] * sv[k]) * S;
            corner[k][3] = su[k] < 0 ? e : 1.f - e;
            corner[k][4] = sv[k] > 0 ? e : 1.f - e;
        }
        const int order[6] = {0, 1, 2, 0, 2, 3};
        float v[6 * 9];
        for (int i = 0; i < 6; i++) {
            const float* p = corner[order[i]];
            float* o = &v[i * 9];
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; o[4] = p[4];
            o[5] = o[6] = o[7] = o[8] = 1.f;
        }
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (void*)12);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, (void*)20);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void gfxBegin2D(int w, int h) {
    s_w = w; s_h = h;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_prog);
    Mat4 ortho;
    ortho.m[0] = 2.f / w; ortho.m[5] = -2.f / h; ortho.m[10] = -1; ortho.m[15] = 1;
    ortho.m[12] = -1; ortho.m[13] = 1;
    glUniformMatrix4fv(uMVP, 1, GL_FALSE, ortho.m);
    glUniform1f(uFogDen, 0.f);
    glUniform1f(uAlpha, 0.f);
    glUniform4f(uTint, 1, 1, 1, 1);
    glUniform4f(uST, 1, -1, 0, 1);   // 2D uses image-space UVs
    glUniform1f(uColScale, 1.f);
    Mat4 id;
    glUniformMatrix4fv(uModel, 1, GL_FALSE, id.m);
    s_quad.clear();
}

void gfxEndFrame() { }

void gfxDrawRect(float x, float y, float w, float h, const Color& tint, Texture* tex, float u0, float v0, float u1, float v1) {
    // flush previous batch
    if (!s_quad.empty()) {
        // (handled below by batching per call; simpler: immediate draw)
    }
    if (tex) uploadTexture(tex);
    float v[] = {
        x, y, u0, v0, tint.r, tint.g, tint.b, tint.a,
        x + w, y, u1, v0, tint.r, tint.g, tint.b, tint.a,
        x + w, y + h, u1, v1, tint.r, tint.g, tint.b, tint.a,
        x, y, u0, v0, tint.r, tint.g, tint.b, tint.a,
        x + w, y + h, u1, v1, tint.r, tint.g, tint.b, tint.a,
        x, y + h, u0, v1, tint.r, tint.g, tint.b, tint.a,
    };
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_2D, tex ? tex->gl : s_white);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)8);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void*)16);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
}

// quad of size w x h centred on (cx, cy), its top edge facing screen direction `ang`
// (radians, 0 = up, clockwise positive)
void gfxDrawQuadRot(float cx, float cy, float w, float h, float ang, const Color& tint, Texture* tex) {
    if (tex) uploadTexture(tex);
    float ux = sinf(ang), uy = -cosf(ang);       // towards the top edge
    float rx = -uy, ry = ux;                       // towards the right edge
    float hw = w * 0.5f, hh = h * 0.5f;
    float ax = cx - rx * hw + ux * hh, ay = cy - ry * hw + uy * hh;   // top-left
    float bx = cx + rx * hw + ux * hh, by = cy + ry * hw + uy * hh;   // top-right
    float c2x = cx + rx * hw - ux * hh, c2y = cy + ry * hw - uy * hh; // bottom-right
    float dx = cx - rx * hw - ux * hh, dy = cy - ry * hw - uy * hh;   // bottom-left
    float v[] = {
        ax, ay, 0, 0, tint.r, tint.g, tint.b, tint.a,
        bx, by, 1, 0, tint.r, tint.g, tint.b, tint.a,
        c2x, c2y, 1, 1, tint.r, tint.g, tint.b, tint.a,
        ax, ay, 0, 0, tint.r, tint.g, tint.b, tint.a,
        c2x, c2y, 1, 1, tint.r, tint.g, tint.b, tint.a,
        dx, dy, 0, 1, tint.r, tint.g, tint.b, tint.a,
    };
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glBindTexture(GL_TEXTURE_2D, tex ? tex->gl : s_white);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)8);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void*)16);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
}

// offscreen colour + depth target (the minimap camera's RenderTexture)
static GLuint s_rtFbo = 0, s_rtDepth = 0;
static Texture s_rtTex;
static GLint s_savedVp[4] = {0, 0, 1280, 720};
Texture* gfxOffscreenBegin(int size) {
    if (!s_rtFbo) {
        glGenTextures(1, &s_rtTex.gl);
        glBindTexture(GL_TEXTURE_2D, s_rtTex.gl);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenRenderbuffers(1, &s_rtDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, s_rtDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, size, size);
        glGenFramebuffers(1, &s_rtFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_rtFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_rtTex.gl, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_rtDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) printf("[gl] offscreen target incomplete\n");
        s_rtTex.w = s_rtTex.h = size; s_rtTex.id = 9990;
        snprintf(s_rtTex.name, sizeof s_rtTex.name, "minimap_rt");
    }
    glGetIntegerv(GL_VIEWPORT, s_savedVp);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, s_rtFbo);
    glViewport(0, 0, s_rtTex.w, s_rtTex.h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(s_prog);
    glUniform1f(uFogDen, 0.f);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uTex, 0);
    return &s_rtTex;
}
static bool s_scissorOn = false;
void gfxOffscreenEnd() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(s_savedVp[0], s_savedVp[1], s_savedVp[2], s_savedVp[3]);
    if (s_scissorOn) glEnable(GL_SCISSOR_TEST);
}

// PC window: clear the whole window black, then draw the 16:9 game area inside it
// (letterbox); the scissor keeps later clears inside the game area
void gfxSetScreenViewport(int x, int y, int w, int h, int winW, int winH) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, winW, winH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(x, y, w, h);
    glScissor(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
    s_scissorOn = true;
}

// textured quad whose alpha is multiplied by `mask` sampled over the same 0..1 UVs
void gfxDrawRectMaskedUV(float x, float y, float w, float h, const Color& tint, Texture* tex, Texture* mask,
                         float u0, float v0, float u1, float v1) {
    if (!mask) { gfxDrawRect(x, y, w, h, tint, tex, u0, v0, u1, v1); return; }
    uploadTexture(mask);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mask->gl);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(uMaskOn, 1.f);
    gfxDrawRect(x, y, w, h, tint, tex, u0, v0, u1, v1);
    glUniform1f(uMaskOn, 0.f);
}

void gfxDrawRectMasked(float x, float y, float w, float h, const Color& tint, Texture* tex, Texture* mask) {
    if (!mask) { gfxDrawRect(x, y, w, h, tint, tex); return; }
    uploadTexture(mask);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mask->gl);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(uMaskOn, 1.f);
    gfxDrawRect(x, y, w, h, tint, tex);
    glUniform1f(uMaskOn, 0.f);
}

static const Glyph* findGlyph(uint32_t code) {
    for (auto& g : g_glyphs) if (g.code == code) return &g;
    return nullptr;
}

void gfxDrawText(float x, float y, float scale, const char* text, const Color& tint) {
    Texture* atlas = getTexture(g_fontTex);
    if (!atlas) return;
    float cx = x;
    float aw = (float)atlas->w, ah = (float)atlas->h;
    for (const char* p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') { cx = x; y += g_fontPixelSize * scale; continue; }
        const Glyph* g = findGlyph(c);
        if (!g) { cx += g_fontPixelSize * 0.3f * scale; continue; }
        gfxDrawRect(cx + g->bx * scale, y + g->by * scale,
                    (float)g->w * scale, (float)g->h * scale, tint, atlas,
                    (float)g->x / aw, (float)g->y / ah,
                    (float)(g->x + g->w) / aw, (float)(g->y + g->h) / ah);
        cx += (float)(g->w + 2) * scale;
    }
}

float gfxTextWidth(const char* text, float scale) {
    float w = 0, best = 0;
    for (const char* p = text; *p; p++) {
        if (*p == '\n') { best = std::max(best, w); w = 0; continue; }
        const Glyph* g = findGlyph((unsigned char)*p);
        w += (g ? (float)(g->w + 2) : g_fontPixelSize * 0.3f) * scale;
    }
    return std::max(best, w);
}

// ------------------------------------------------------------------ audio
struct Voice {
    const AudioClip* clip = nullptr;
    double pos = 0;          // in clip frames (fractional: clips are resampled)
    float vol = 1.f;
    bool loop = false;
    bool active = false;
};
static const int MAX_VOICES = 16;
static Voice s_voices[MAX_VOICES];
static SDL_AudioDeviceID s_dev = 0;
static SDL_AudioSpec s_have;
static float s_master = 1.f;
#ifdef __SWITCH__
// SDL's Switch audio backend opens but stays silent here: feed audout directly
static Mutex s_audMutex;
static Thread s_audThread;
static volatile bool s_audRun = false;
static bool s_audOut = false;
#define AUD_LOCK() mutexLock(&s_audMutex)
#define AUD_UNLOCK() mutexUnlock(&s_audMutex)
#else
#define AUD_LOCK() do { if (s_dev) SDL_LockAudioDevice(s_dev); } while (0)
#define AUD_UNLOCK() do { if (s_dev) SDL_UnlockAudioDevice(s_dev); } while (0)
#endif

// mixes every voice into whatever format the device accepted (Switch: 48 kHz stereo)
static void audioCallback(void* ud, Uint8* stream, int len) {
    (void)ud;
    int16_t* out = (int16_t*)stream;
    int outCh = s_have.channels > 0 ? s_have.channels : 1;
    int frames = len / (2 * outCh);
    memset(out, 0, len);
    static float acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    for (int i = 0; i < frames * 2; i++) acc[i] = 0;
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice& vo = s_voices[v];
        if (!vo.active || !vo.clip || vo.clip->frames <= 0) continue;
        const AudioClip* c = vo.clip;
        double step = (double)c->freq / (double)(s_have.freq > 0 ? s_have.freq : 44100);
        float g = vo.vol * s_master;
        for (int i = 0; i < frames; i++) {
            if (vo.pos >= c->frames) {
                if (vo.loop) vo.pos -= c->frames; else { vo.active = false; break; }
            }
            size_t f = (size_t)vo.pos;
            size_t f1 = f + 1 < (size_t)c->frames ? f + 1 : (vo.loop ? 0 : f);
            float fr = (float)(vo.pos - (double)f);   // linear interpolation (no aliasing)
            if (c->ch >= 2) {
                float l0 = c->pcm[f * c->ch], l1 = c->pcm[f1 * c->ch];
                float r0 = c->pcm[f * c->ch + 1], r1 = c->pcm[f1 * c->ch + 1];
                acc[i * 2] += (l0 + (l1 - l0) * fr) * g;
                acc[i * 2 + 1] += (r0 + (r1 - r0) * fr) * g;
            } else {
                float s0 = c->pcm[f], s1 = c->pcm[f1];
                float sm = (s0 + (s1 - s0) * fr) * g;
                acc[i * 2] += sm; acc[i * 2 + 1] += sm;
            }
            vo.pos += step;
        }
    }
    for (int i = 0; i < frames; i++) {
        float l = acc[i * 2], r = acc[i * 2 + 1];
        if (outCh == 1) out[i] = (int16_t)std::max(-32767.f, std::min(32767.f, (l + r) * 0.5f));
        else {
            out[i * outCh] = (int16_t)std::max(-32767.f, std::min(32767.f, l));
            out[i * outCh + 1] = (int16_t)std::max(-32767.f, std::min(32767.f, r));
            for (int k = 2; k < outCh; k++) out[i * outCh + k] = 0;
        }
    }
}

static void audioLog(const char* msg) {
    printf("%s\n", msg);
}

#ifdef __SWITCH__
static void audThreadMain(void*) {
    const int FR = 512;                                    // ~10.7 ms per buffer at 48 kHz (low latency)
    const size_t dataSize = FR * 2 * sizeof(int16_t);
    const size_t bufSize = (dataSize + 0xfff) & ~(size_t)0xfff;
    static AudioOutBuffer bufs[3];
    for (int i = 0; i < 3; i++) {
        void* mem = memalign(0x1000, bufSize);
        memset(mem, 0, bufSize);
        bufs[i].next = nullptr; bufs[i].buffer = mem; bufs[i].buffer_size = bufSize;
        bufs[i].data_size = dataSize; bufs[i].data_offset = 0;
        mutexLock(&s_audMutex);
        audioCallback(nullptr, (Uint8*)mem, (int)dataSize);
        mutexUnlock(&s_audMutex);
        audoutAppendAudioOutBuffer(&bufs[i]);
    }
    while (s_audRun) {
        AudioOutBuffer* rel = nullptr;
        u32 cnt = 0;
        if (R_FAILED(audoutWaitPlayFinish(&rel, &cnt, 100000000ULL)) || !rel) continue;
        mutexLock(&s_audMutex);
        audioCallback(nullptr, (Uint8*)rel->buffer, (int)rel->data_size);
        mutexUnlock(&s_audMutex);
        armDCacheFlush(rel->buffer, rel->buffer_size);
        {
            // diagnostics: peak level of what goes out, a few times early on
            static int nbuf = 0;
            nbuf++;
            if (nbuf == 50 || nbuf == 200 || nbuf == 500 || nbuf == 1500) {
                const int16_t* sm = (const int16_t*)rel->buffer;
                int peak = 0, act = 0;
                for (size_t k = 0; k < rel->data_size / 2; k++) peak = std::max(peak, (int)abs(sm[k]));
                for (int v = 0; v < MAX_VOICES; v++) if (s_voices[v].active) act++;
                char b[128];
                snprintf(b, sizeof b, "[audio] buffer %d peak %d voices %d master %.2f", nbuf, peak, act, s_master);
                audioLog(b);
            }
        }
        audoutAppendAudioOutBuffer(rel);
    }
}
#endif

bool audioInit() {
#ifdef __SWITCH__
    {
        char buf[160];
        mutexInit(&s_audMutex);
        Result rc = audoutInitialize();
        if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
        if (R_SUCCEEDED(rc)) {
            s_have.freq = (int)audoutGetSampleRate();
            s_have.channels = (Uint8)audoutGetChannelCount();
            if (s_have.freq <= 0) s_have.freq = 48000;
            if (s_have.channels == 0) s_have.channels = 2;
            s_audRun = true;
            if (R_SUCCEEDED(threadCreate(&s_audThread, audThreadMain, nullptr, nullptr, 0x8000, 0x2B, -2)) &&
                R_SUCCEEDED(threadStart(&s_audThread))) {
                s_audOut = true;
                snprintf(buf, sizeof buf, "[audio] audout %d Hz %d ch, %d clips", s_have.freq, s_have.channels, (int)g_audio.size());
                audioLog(buf);
                return true;
            }
            s_audRun = false;
        }
        snprintf(buf, sizeof buf, "[audio] audout failed 0x%x, trying SDL", (unsigned)rc);
        audioLog(buf);
    }
#endif
    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audioCallback;
    // keep 16-bit samples, take the device's own rate / channel count
    s_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &s_have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    char buf[160];
    if (!s_dev) {
        snprintf(buf, sizeof buf, "[audio] open failed: %s (driver %s)", SDL_GetError(), SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
        audioLog(buf);
        return false;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    snprintf(buf, sizeof buf, "[audio] driver %s, %d Hz %d ch, %d clips", SDL_GetCurrentAudioDriver(), s_have.freq, s_have.channels, (int)g_audio.size());
    audioLog(buf);
    return true;
}

void audioPlay(const char* name, float volume, bool loop) {
    AudioClip* c = getAudio(name);
    if (!c) { printf("[audio] missing clip %s\n", name); return; }
    AUD_LOCK();
    int slot = -1;
    for (int i = 0; i < MAX_VOICES; i++) if (!s_voices[i].active) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < MAX_VOICES; i++) if (!s_voices[i].loop) { slot = i; break; }   // never steal the music
    if (slot >= 0) {
        Voice& v = s_voices[slot];
        v.clip = c; v.pos = 0; v.vol = volume; v.loop = loop; v.active = true;
    }
    AUD_UNLOCK();
}

void audioStopMusic() {
    AUD_LOCK();
    for (int i = 0; i < MAX_VOICES; i++) if (s_voices[i].loop) s_voices[i].active = false;
    AUD_UNLOCK();
}
void audioSetVolume(float v) { s_master = clampf(v, 0, 1); }
// stop the mixer callback before globals are torn down
void audioShutdown() {
#ifdef __SWITCH__
    if (s_audOut) {
        s_audRun = false;
        threadWaitForExit(&s_audThread);
        threadClose(&s_audThread);
        audoutStopAudioOut();
        audoutExit();
        s_audOut = false;
    }
#endif
    if (s_dev) { SDL_CloseAudioDevice(s_dev); s_dev = 0; }
}
void audioUpdate() {}
