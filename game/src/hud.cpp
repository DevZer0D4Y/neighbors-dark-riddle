// Dark Riddle - Nintendo Switch port. The original in-game Canvas (romfs:/hud.pak,
// written by work/make_hud.py): sprites, layout and texts of every level's UI.
#include "port.h"

struct HudGlyph { int x = 0, y = 0, w = 0, h = 0; float bx = 0, by = 0, adv = 0; bool ok = false; };
struct HudFont {
    std::string name;
    float px = 64, ascent = 50, lineH = 70;
    int tex = -1;
    HudGlyph g[256];
};

static std::vector<HudFont> s_fonts;
static std::vector<HudElement> s_elems;
static int s_texBase = 3000;

static bool rd(const std::vector<uint8_t>& b, size_t& p, void* out, size_t n) {
    if (p + n > b.size()) return false;
    memcpy(out, &b[p], n); p += n; return true;
}
static uint32_t ru32(const std::vector<uint8_t>& b, size_t& p) { uint32_t v = 0; rd(b, p, &v, 4); return v; }
static float rf32(const std::vector<uint8_t>& b, size_t& p) { float v = 0; rd(b, p, &v, 4); return v; }
static std::string rstr(const std::vector<uint8_t>& b, size_t& p) {
    uint32_t n = ru32(b, p);
    if (p + n > b.size()) { p = b.size(); return ""; }
    std::string s((const char*)&b[p], n); p += n; return s;
}
static std::string rname(const std::vector<uint8_t>& b, size_t& p) {
    char n[33] = {0}; rd(b, p, n, 32); return n;
}

bool hudLoad(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[hud] cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b((size_t)sz);
    size_t got = fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (got != b.size() || b.size() < 8 || memcmp(b.data(), "DRHD", 4) != 0) { printf("[hud] bad file\n"); return false; }
    size_t p = 8;
    uint32_t nTex = ru32(b, p);
    for (uint32_t i = 0; i < nTex; i++) {
        Texture t;
        t.w = (int)ru32(b, p); t.h = (int)ru32(b, p);
        uint32_t bytes = ru32(b, p);
        std::string nm = rname(b, p);
        snprintf(t.name, sizeof t.name, "%s", nm.c_str());
        if (p + bytes > b.size()) return false;
        t.data.assign(&b[p], &b[p] + bytes); p += bytes;
        t.id = s_texBase + (int)i;
        g_textures.push_back(std::move(t));
    }
    uint32_t nFont = ru32(b, p);
    s_fonts.resize(nFont);
    for (uint32_t i = 0; i < nFont; i++) {
        HudFont& hf = s_fonts[i];
        hf.name = rname(b, p);
        hf.px = rf32(b, p);
        hf.tex = s_texBase + (int)ru32(b, p);
        hf.ascent = rf32(b, p); hf.lineH = rf32(b, p);
        uint32_t ng = ru32(b, p);
        for (uint32_t k = 0; k < ng; k++) {
            uint32_t code = ru32(b, p);
            HudGlyph g;
            g.x = (int)ru32(b, p); g.y = (int)ru32(b, p); g.w = (int)ru32(b, p); g.h = (int)ru32(b, p);
            g.bx = rf32(b, p); g.by = rf32(b, p); g.adv = rf32(b, p);
            g.ok = true;
            if (code < 256) hf.g[code] = g;
        }
    }
    uint32_t nEl = ru32(b, p);
    s_elems.resize(nEl);
    for (uint32_t i = 0; i < nEl; i++) {
        HudElement& e = s_elems[i];
        e.path = rstr(b, p);
        size_t slash = e.path.rfind('/');
        e.name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
        e.level = (int)ru32(b, p);
        uint32_t kind = ru32(b, p);
        e.kind = (int)(kind & 0xff);
        e.activeByDefault = (kind & 0x100) == 0;
        e.x = rf32(b, p); e.y = rf32(b, p); e.w = rf32(b, p); e.h = rf32(b, p);
        e.color.r = rf32(b, p); e.color.g = rf32(b, p); e.color.b = rf32(b, p); e.color.a = rf32(b, p);
        e.sprite = rname(b, p);
        e.font = (int)ru32(b, p);
        e.fontPx = rf32(b, p); e.minPx = rf32(b, p);
        e.align = (int)ru32(b, p);
        e.imageType = (int)ru32(b, p);
        e.text = rstr(b, p);
    }
    printf("[hud] %u textures, %u fonts, %u elements\n", nTex, nFont, nEl);
    return true;
}

const std::vector<HudElement>& hudElements() { return s_elems; }

const HudElement* hudFind(int level, const char* path) {
    for (auto& e : s_elems) if (e.level == level && e.path == path) return &e;
    return nullptr;
}

void hudDrawImage(const HudElement& e, const Color& tint, float scale) {
    Texture* t = e.sprite.empty() ? nullptr : findTexture(e.sprite.c_str());
    float w = e.w * scale, h = e.h * scale;
    float x = e.x + (e.w - w) * 0.5f, y = e.y + (e.h - h) * 0.5f;
    Color c(e.color.r * tint.r, e.color.g * tint.g, e.color.b * tint.b, e.color.a * tint.a);
    gfxDrawRect(x, y, w, h, c, t);
}

// ------------------------------------------------------------------ text
static float wordWidth(const HudFont& f, const std::string& s, size_t a, size_t b, float sc) {
    float w = 0;
    for (size_t i = a; i < b; i++) { unsigned char c = (unsigned char)s[i]; if (f.g[c].ok) w += f.g[c].adv * sc; }
    return w;
}

// greedy word wrap into lines that fit maxW
static void wrapLines(const HudFont& f, const std::string& s, float sc, float maxW, std::vector<std::string>& out) {
    out.clear();
    size_t i = 0;
    std::string line;
    float lineW = 0, spaceW = f.g[(unsigned char)' '].ok ? f.g[(unsigned char)' '].adv * sc : sc * 16;
    while (i <= s.size()) {
        if (i == s.size() || s[i] == '\n') {
            out.push_back(line); line.clear(); lineW = 0;
            if (i == s.size()) break;
            i++; continue;
        }
        if (s[i] == ' ') { i++; continue; }
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\n') j++;
        float ww = wordWidth(f, s, i, j, sc);
        if (!line.empty() && lineW + spaceW + ww > maxW) { out.push_back(line); line.clear(); lineW = 0; }
        if (!line.empty()) { line += ' '; lineW += spaceW; }
        line.append(s, i, j - i); lineW += ww;
        i = j;
    }
}

void hudDrawText(const HudElement& e, const char* textOverride, const Color& tint) {
    if (e.font < 0 || e.font >= (int)s_fonts.size()) return;
    const HudFont& f = s_fonts[e.font];
    Texture* atlas = getTexture(f.tex);
    if (!atlas) return;
    std::string s = textOverride ? textOverride : e.text;
    if (s.empty()) return;
    // Unity Best Fit: largest size in [min, max] whose wrapped text fits the rect
    float px = e.fontPx;
    std::vector<std::string> lines;
    for (;;) {
        float sc = px / f.px;
        wrapLines(f, s, sc, e.w, lines);
        float hTot = lines.size() * f.lineH * sc;
        float wMax = 0;
        for (auto& l : lines) wMax = std::max(wMax, wordWidth(f, l, 0, l.size(), sc));
        if (e.minPx <= 0 || px <= e.minPx || (hTot <= e.h && wMax <= e.w + 0.5f)) break;
        px -= 1.f;
    }
    float sc = px / f.px;
    float lh = f.lineH * sc;
    float hTot = lines.size() * lh;
    int col = e.align % 3, row = e.align / 3;
    float y = row == 0 ? e.y : row == 1 ? e.y + (e.h - hTot) * 0.5f : e.y + e.h - hTot;
    Color c(e.color.r * tint.r, e.color.g * tint.g, e.color.b * tint.b, e.color.a * tint.a);
    float aw = (float)atlas->w, ah = (float)atlas->h;
    for (auto& l : lines) {
        float lw = wordWidth(f, l, 0, l.size(), sc);
        float x = col == 0 ? e.x : col == 1 ? e.x + (e.w - lw) * 0.5f : e.x + e.w - lw;
        for (unsigned char ch : l) {
            const HudGlyph& g = f.g[ch];
            if (!g.ok) continue;
            if (g.w > 0)
                gfxDrawRect(x + g.bx * sc, y + g.by * sc, g.w * sc, g.h * sc, c, atlas,
                            g.x / aw, g.y / ah, (g.x + g.w) / aw, (g.y + g.h) / ah);
            x += g.adv * sc;
        }
        y += lh;
    }
}

bool hudHit(const HudElement& e, float px, float py) {
    return px >= e.x && px <= e.x + e.w && py >= e.y && py <= e.y + e.h;
}

int hudFontIndex(const char* name) {
    for (size_t i = 0; i < s_fonts.size(); i++) if (s_fonts[i].name == name) return (int)i;
    return -1;
}
