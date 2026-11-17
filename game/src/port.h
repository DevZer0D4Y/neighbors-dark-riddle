// Dark Riddle - Nintendo Switch port. Shared engine declarations.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

// ------------------------------------------------------------------ math
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() {}
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    float len() const { return sqrtf(x * x + y * y + z * z); }
    Vec3 norm() const { float l = len(); return l > 1e-6f ? Vec3(x / l, y / l, z / l) : Vec3(0, 0, 0); }
};
static inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    Color() {}
    Color(float R, float G, float B, float A = 1.f) : r(R), g(G), b(B), a(A) {}
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Mat4 {
    float m[16];  // column-major
    Mat4() { for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f; }
    Mat4 mul(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * o.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        return Vec3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                    m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                    m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
    }
};

Mat4 matTranslate(const Vec3& t);
Mat4 matScale(const Vec3& s);
Mat4 matRotY(float a);
Mat4 matFromQuat(const Quat& q);
Mat4 matPerspective(float fovyDeg, float aspect, float n, float f);
Mat4 matLookAt(const Vec3& eye, const Vec3& center, const Vec3& up);

// ------------------------------------------------------------------ assets
struct Texture {
    int id = 0, w = 0, h = 0, fmt = 0;
    char name[40] = {0};
    std::vector<uint8_t> data;
    unsigned int gl = 0;
};

struct Material {
    int texId = 0;
    float r = 1, g = 1, b = 1, a = 1;
    float tx = 1, ty = 1, ox = 0, oy = 0;
    int cutout = 0;          // 0 opaque, 1 alpha test, 2 alpha blend
    float cutoff = 0.5f;     // Unity _Cutoff for alpha-tested materials
};

struct Mesh {
    int id = 0, matId = 0;
    int vcount = 0, icount = 0;
    std::vector<uint8_t> verts;    // 24 bytes per vertex
    std::vector<uint32_t> idx;
    unsigned int vbo = 0, ibo = 0;
    bool dirty = false;               // verts changed (skinned meshes): re-upload
};

struct Collider {
    Vec3 mn, mx;
};

struct Prop {
    int type = 0;   // 1 door, 2 takeable, 3 neighbour/prop
    char name[40] = {0};
    Vec3 pos;
    Quat rot;
    Vec3 scale{1, 1, 1};
    int meshId = 0;
    Vec3 half;
    Vec3 center;
    Quat restRot;   // original world rotation (already baked into the mesh)
    int parent = -1;   // type 13: extra sub-mesh that moves with props[parent]
};

struct Marker {
    int type = 0;   // 1 spawn, 2 neighbour, 3 waypoint, 4 home, 5 car, 6 wires, 7 key, 8 scissors, 9 money
    Vec3 pos;
    float radius = 1;
    char name[40] = {0};
    Vec3 half;      // trigger box half extents (Home / Car / Wires), centred on pos
};

struct LightSrc {
    Vec3 pos, dir, color;
    float intensity = 1, range = 10;
    int type = 2;
};

struct Scene {
    int level = 1;
    Vec3 spawn;
    float yaw = 0;
    Vec3 ambient{.2f, .2f, .25f}, fog{.05f, .05f, .07f};
    std::vector<int> meshIds;
    std::vector<Collider> colliders;
    std::vector<Prop> props;
    std::vector<Marker> markers;
    std::vector<LightSrc> lights;
    // broadphase
    static const int GRID = 96;
    static constexpr float CELL = 8.0f;
    std::vector<int> grid[GRID * GRID];
};

struct AudioClip {
    std::string name;
    int ch = 1, freq = 44100, frames = 0;
    std::vector<int16_t> pcm;
};

struct Glyph {
    uint32_t code = 0;
    int x = 0, y = 0, w = 0, h = 0;
    float bx = 0, by = 0, base = 0;
};

// ------------------------------------------------------------------ globals
extern std::vector<Texture> g_textures;
extern std::vector<Material> g_materials;
extern std::vector<Mesh> g_meshes;
extern std::vector<Scene> g_scenes;
extern std::vector<AudioClip> g_audio;
extern std::vector<Glyph> g_glyphs;
extern int g_fontTex;
extern float g_fontPixelSize;
extern Vec3 g_switchLight;

// ------------------------------------------------------------------ engine api
bool engineLoadPak(const char* path);

// Menu UI exported from the game's own Menu scene (sprites + RectTransform layout)
struct UIElement {
    int panel = 0;
    char tex[32] = {0};
    float cx = 0, cy = 0, w = 0, h = 0;
    int id = 0;
    char label[24] = {0};
};
bool uiLoadPak(const char* path);

// In-game Canvas exported from every level (romfs:/hud.pak, work/make_hud.py)
struct HudElement {
    std::string path, name, sprite, text;
    int level = 0, kind = 0;          // kind: 0 rect, 1 image, 2 text, 3 raw image
    bool activeByDefault = true;
    float x = 0, y = 0, w = 0, h = 0; // pixels at 1280x720, y down
    Color color;
    int font = -1, align = 4, imageType = 0;
    float fontPx = 0, minPx = 0;
};
// The neighbour character (romfs:/char.pak, work/make_char.py)
struct CharLevel {
    int level = 0;
    bool present = false, active = false;
    Vec3 pos;
    float yaw = 0;
    float seeDistance = 10, attackDistance = 0.5f;
    std::vector<Vec3> waypoints;    // NeighborAI.waypoints, in order
};
struct CharAnim { int state = 0, prevState = 0; float t = 0, prevT = 0, blend = 1; };
bool charLoad(const char* path);
const CharLevel* charLevel(int level);
void charAnimUpdate(CharAnim& a, float dt, float walkSpeed, bool dizzy);
void charDraw(const CharAnim& a, const Vec3& pos, float yaw);
void charGameOverCamera(const Vec3& pos, float yaw, Vec3& eye, Vec3& fwd, float& fov);

// Baked NavMesh (romfs:/nav.pak, work/make_nav.py)
bool navLoad(const char* path);
bool navReady();
int navFindPoly(const Vec3& p, Vec3* snapped);
bool navPath(const Vec3& from, const Vec3& to, std::vector<Vec3>& out);

bool hudLoad(const char* path);
const std::vector<HudElement>& hudElements();
const HudElement* hudFind(int level, const char* path);
void hudDrawImage(const HudElement& e, const Color& tint, float scale = 1.f);
void hudDrawText(const HudElement& e, const char* textOverride, const Color& tint);
bool hudHit(const HudElement& e, float px, float py);
int hudFontIndex(const char* name);
const std::vector<UIElement>& uiElements();
Texture* getTexture(int id);
Texture* findTexture(const char* name);
Mesh* getMesh(int id);
Material* getMaterial(int id);
AudioClip* getAudio(const char* name);

bool gfxInit(int w, int h);
void gfxBeginFrame(const Vec3& ambient, const Vec3& fog, float fogDensity);
void gfxSetCamera(const Mat4& viewProj);
void gfxSetCameraPos(const Vec3& p);
void gfxDrawMesh(Mesh* m, const Mat4& model, const Color& tint, float alphaTest, bool twoSided = false);
void gfxEnd3D();
unsigned int gfxSkinUpload(const float* verts16, int nv, const uint32_t* idx, int ni, unsigned int* iboOut);
void gfxDrawSkinned(unsigned int vbo, unsigned int ibo, int ni, int matId, const float* boneRows, int nb, const Mat4& model);
extern float g_skinFlatLight;   // >0: skinned draw uses this flat light (trees)
bool treesLoad(const char* path);
void treesDraw(int level, float time);
void gfxDrawSkybox(const Mat4& viewProjNoTranslate);
void gfxBegin2D(int w, int h);
void gfxDrawRect(float x, float y, float w, float h, const Color& tint, Texture* tex, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
void gfxDrawRectMasked(float x, float y, float w, float h, const Color& tint, Texture* tex, Texture* mask);
void gfxDrawRectMaskedUV(float x, float y, float w, float h, const Color& tint, Texture* tex, Texture* mask,
                         float u0, float v0, float u1, float v1);
void gfxDrawQuadRot(float cx, float cy, float w, float h, float ang, const Color& tint, Texture* tex);
Texture* gfxOffscreenBegin(int size);
void gfxOffscreenEnd();
void gfxSetScreenViewport(int x, int y, int w, int h, int winW, int winH);
Mat4 matOrtho(float l, float r, float b, float t, float n, float f);
void gfxDrawText(float x, float y, float scale, const char* text, const Color& tint);
float gfxTextWidth(const char* text, float scale);
void gfxEndFrame();

// audio
bool audioInit();
void audioPlay(const char* name, float volume = 1.f, bool loop = false);
void audioStopMusic();
void audioSetVolume(float v);
void audioShutdown();
void audioUpdate();
