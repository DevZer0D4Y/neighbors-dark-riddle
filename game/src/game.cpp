// Dark Riddle - Nintendo Switch port.
// Native reimplementation of the original Unity game logic (Assembly-CSharp),
// running the original levels, meshes, textures and audio.
#include "port.h"

#include <SDL2/SDL.h>
#include "gl_port.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#endif

static int W = 1280, H = 720;
static float g_time = 0;

enum State { ST_BOOT, ST_MENU, ST_LOADING, ST_INTRO, ST_PLAY, ST_TASKS, ST_WON, ST_DEAD };
static State state = ST_BOOT;
static State g_lastState = ST_BOOT;
static float stateT = 0;

// ------------------------------------------------------------------ input
struct Input {
    float mx = 0, my = 0, rx = 0, ry = 0;
    bool a = false, b = false, x = false, y = false, start = false, run = false;
    bool aPress = false, bPress = false, xPress = false, yPress = false, startPress = false;
    bool anyPress = false;
    bool tap = false; float tapX = 0, tapY = 0;   // touch tap this frame, screen pixels
    bool dpadPress[4] = {false, false, false, false};  // up, down, left, right (edge)
    float lookX = 0, lookY = 0;
    bool hover = false; float hoverX = 0, hoverY = 0;   // mouse over the UI (game coords)
};
static Input in;

static bool prevA = false, prevB = false, prevX = false, prevY = false, prevStart = false;

static SDL_GameController* g_pad = nullptr;
static int g_panelSel = 0;      // selected button of the in-game panels

struct RectF { float x = 0, y = 0, w = 0, h = 0; };
static long long g_stickFinger = -1, g_runFinger = -1;
static float g_stickOX = 0, g_stickOY = 0, g_stickX = 0, g_stickY = 0;
static RectF g_runRect;   // RunButton rect of the current level (set by the HUD)
// PlayerPrefs "ControlType" (Menu options): 0 = swipe look (DualTouchControls),
// 1 = look joystick (MobileJoystick1)
static int g_ctrlType = 0;
static RectF g_look1Rect;  // MobileJoystick1 rect (set by the HUD)
static long long g_lookFinger = -1;
static float g_lookOX = 0, g_lookOY = 0, g_lookX = 0, g_lookY = 0;
static bool g_touchHeld = false;   // any finger down (menu slider drag)
static float g_touchX = 0, g_touchY = 0;

// clean exit: the audio callback must stop before static data is destroyed
[[noreturn]] static void quitGame() {
    audioShutdown();
    SDL_Quit();
#ifdef __SWITCH__
    romfsExit();
#endif
    exit(0);
}

// ------------------------------------------------------------------ bindings
// Rebindable actions (Controls editor). Keyboard codes: SDL scancodes, BK_MOUSE+n for
// mouse button n, BK_WHEELUP/DN for the wheel. Controller codes: SDL controller
// buttons, BP_LT/BP_RT for the triggers. -1 = unbound.
enum { A_FWD, A_BACK, A_LEFT, A_RIGHT, A_JUMP, A_RUN, A_USE, A_THROW, A_TASKS, A_PAUSE, A_N };
static const char* const kActName[A_N] = {"MOVE FORWARD", "MOVE BACK", "MOVE LEFT", "MOVE RIGHT", "JUMP",
                                          "RUN", "USE / PICK UP", "THROW", "TASKS", "PAUSE"};
enum { BK_MOUSE = 600, BK_WHEELUP = 700, BK_WHEELDN = 701 };
enum { BP_LT = 100, BP_RT = 101 };
struct Bindings { int key[A_N]; int pad[A_N]; };
static Bindings g_bind;
#ifdef __SWITCH__
static const char* const kSchemeName[] = {"STANDARD", "ALTERNATIVE"};
#else
static const char* const kSchemeName[] = {"DEFAULT", "CLASSIC", "ALTERNATIVE PAD"};
#endif
static const int SCHEME_N = (int)(sizeof(kSchemeName) / sizeof(kSchemeName[0]));   // == custom
static int g_scheme = 0;
static float g_sens = 1.f;             // look sensitivity (mouse and right stick)
static const float SENS_MIN = 0.2f, SENS_MAX = 3.0f;

// controls editor capture (menu page 3)
static bool g_edCapture = false;
static int g_capKey = -1, g_capPad = -1;    // first key / pad press this frame (capture)

// PC window: the 1280x720 game area inside the window (letterboxed), window pixels
static int g_vpX = 0, g_vpY = 0, g_vpW = 1280, g_vpH = 720;
static bool g_mouseCaptured = false;
static bool g_wheelUp = false, g_wheelDn = false;
static Uint32 g_mouseBtns = 0;

static void padPreset(int* p, bool alt) {
    for (int a = 0; a < A_N; a++) p[a] = -1;
    // SDL buttons are positional (A south, B east, X west, Y north); on Switch the
    // labels are A east, B south, X north, Y west
#ifdef __SWITCH__
    const int L_A = SDL_CONTROLLER_BUTTON_B, L_B = SDL_CONTROLLER_BUTTON_A,
              L_X = SDL_CONTROLLER_BUTTON_Y, L_Y = SDL_CONTROLLER_BUTTON_X;
#else
    const int L_A = SDL_CONTROLLER_BUTTON_A, L_B = SDL_CONTROLLER_BUTTON_B,
              L_X = SDL_CONTROLLER_BUTTON_X, L_Y = SDL_CONTROLLER_BUTTON_Y;
#endif
    p[A_PAUSE] = SDL_CONTROLLER_BUTTON_START;
#ifdef __SWITCH__
    if (!alt) {
        // the original layout: A use, B jump, X throw, Y tasks, R run
        p[A_USE] = L_A; p[A_JUMP] = L_B; p[A_THROW] = L_X; p[A_TASKS] = L_Y;
        p[A_RUN] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    } else {
        p[A_USE] = BP_RT; p[A_THROW] = BP_LT; p[A_JUMP] = L_B;
        p[A_TASKS] = L_Y; p[A_RUN] = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    }
#else
    (void)L_B;
    if (!alt) {
        // usual PC controller layout: A jump, X use, RT throw, L3 run, Y tasks
        p[A_JUMP] = L_A; p[A_USE] = L_X; p[A_THROW] = BP_RT; p[A_TASKS] = L_Y;
        p[A_RUN] = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    } else {
        // triggers: RT use, LT throw, A jump, LB run, Y tasks
        p[A_USE] = BP_RT; p[A_THROW] = BP_LT; p[A_JUMP] = L_A; p[A_TASKS] = L_Y;
        p[A_RUN] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    }
#endif
}

static void keyPreset(int* k, bool classic) {
    k[A_FWD] = SDL_SCANCODE_W; k[A_BACK] = SDL_SCANCODE_S; k[A_LEFT] = SDL_SCANCODE_A; k[A_RIGHT] = SDL_SCANCODE_D;
    k[A_JUMP] = SDL_SCANCODE_SPACE; k[A_RUN] = SDL_SCANCODE_LSHIFT;
    k[A_TASKS] = SDL_SCANCODE_TAB; k[A_PAUSE] = SDL_SCANCODE_ESCAPE;
    if (!classic) { k[A_USE] = BK_MOUSE + SDL_BUTTON_LEFT; k[A_THROW] = BK_MOUSE + SDL_BUTTON_RIGHT; }   // default
    else { k[A_USE] = SDL_SCANCODE_E; k[A_THROW] = SDL_SCANCODE_Q; }
}

static void applyScheme(int s) {
    if (s < 0 || s >= SCHEME_N) return;
    g_scheme = s;
#ifdef __SWITCH__
    keyPreset(g_bind.key, false);
    padPreset(g_bind.pad, s == 1);
#else
    keyPreset(g_bind.key, s == 1);
    padPreset(g_bind.pad, s == 2);
#endif
}

static const char* ctlPath() {
#ifdef __SWITCH__
    return "sdmc:/switch/darkriddle_controls.cfg";
#else
    return "darkriddle_controls.cfg";
#endif
}

static void saveControls() {
    FILE* f = fopen(ctlPath(), "w");
    if (!f) return;
    fprintf(f, "scheme %d\nsens %.2f\n", g_scheme, g_sens);
    for (int a = 0; a < A_N; a++) fprintf(f, "bind %d %d %d\n", a, g_bind.key[a], g_bind.pad[a]);
    fclose(f);
}

static void loadControls() {
    applyScheme(0);
    FILE* f = fopen(ctlPath(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int a, k, p; float v;
        if (sscanf(line, "scheme %d", &a) == 1) { if (a >= 0 && a < SCHEME_N) applyScheme(a); else g_scheme = SCHEME_N; }
        else if (sscanf(line, "sens %f", &v) == 1) g_sens = clampf(v, SENS_MIN, SENS_MAX);
        else if (sscanf(line, "bind %d %d %d", &a, &k, &p) == 3 && a >= 0 && a < A_N) {
            g_bind.key[a] = k; g_bind.pad[a] = p;
        }
    }
    fclose(f);
    if (g_scheme < SCHEME_N) applyScheme(g_scheme);
    g_bind.key[A_PAUSE] = SDL_SCANCODE_ESCAPE;              // fixed: always reachable
    g_bind.pad[A_PAUSE] = SDL_CONTROLLER_BUTTON_START;
}

static std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

static std::string keyName(int k) {
    if (k < 0) return "-";
    if (k >= BK_MOUSE && k < BK_WHEELUP) {
        switch (k - BK_MOUSE) {
            case SDL_BUTTON_LEFT: return "LEFT MOUSE";
            case SDL_BUTTON_RIGHT: return "RIGHT MOUSE";
            case SDL_BUTTON_MIDDLE: return "MIDDLE MOUSE";
            default: { char b[24]; snprintf(b, sizeof b, "MOUSE %d", k - BK_MOUSE); return b; }
        }
    }
    if (k == BK_WHEELUP) return "WHEEL UP";
    if (k == BK_WHEELDN) return "WHEEL DOWN";
    const char* n = SDL_GetScancodeName((SDL_Scancode)k);
    return (n && *n) ? upper(n) : "?";
}

static std::string padName(int p) {
    if (p < 0) return "-";
#ifdef __SWITCH__
    switch (p) {
        case SDL_CONTROLLER_BUTTON_A: return "B";
        case SDL_CONTROLLER_BUTTON_B: return "A";
        case SDL_CONTROLLER_BUTTON_X: return "Y";
        case SDL_CONTROLLER_BUTTON_Y: return "X";
        case SDL_CONTROLLER_BUTTON_BACK: return "-";
        case SDL_CONTROLLER_BUTTON_START: return "+";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
        case BP_LT: return "ZL";
        case BP_RT: return "ZR";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L-STICK PRESS";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R-STICK PRESS";
    }
#else
    switch (p) {
        case SDL_CONTROLLER_BUTTON_A: return "A";
        case SDL_CONTROLLER_BUTTON_B: return "B";
        case SDL_CONTROLLER_BUTTON_X: return "X";
        case SDL_CONTROLLER_BUTTON_Y: return "Y";
        case SDL_CONTROLLER_BUTTON_BACK: return "BACK";
        case SDL_CONTROLLER_BUTTON_START: return "START";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
        case BP_LT: return "LT";
        case BP_RT: return "RT";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    }
#endif
    switch (p) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-PAD UP";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-PAD DOWN";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-PAD LEFT";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-PAD RIGHT";
    }
    char b[16]; snprintf(b, sizeof b, "BUTTON %d", p);
    return b;
}

static bool keyDown(int k, const Uint8* kb) {
    if (k < 0) return false;
    if (k >= BK_MOUSE && k < BK_WHEELUP) return g_mouseCaptured && (g_mouseBtns & SDL_BUTTON(k - BK_MOUSE)) != 0;
    if (k == BK_WHEELUP) return g_mouseCaptured && g_wheelUp;
    if (k == BK_WHEELDN) return g_mouseCaptured && g_wheelDn;
    return k < SDL_NUM_SCANCODES && kb[k];
}

static bool padDown(int p) {
    if (!g_pad || p < 0) return false;
    if (p == BP_LT) return SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
    if (p == BP_RT) return SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
    return SDL_GameControllerGetButton(g_pad, (SDL_GameControllerButton)p) != 0;
}

static bool actDown(int a, const Uint8* kb) {
    if (g_edCapture) return false;
    return keyDown(g_bind.key[a], kb) || padDown(g_bind.pad[a]);
}

// window pixels -> 1280x720 game coordinates
static void toGame(int wx, int wy, float& gx, float& gy) {
    gx = (wx - g_vpX) * (float)W / std::max(1, g_vpW);
    gy = (wy - g_vpY) * (float)H / std::max(1, g_vpH);
}

static void toggleFullscreen(SDL_Window* w) {
#ifndef __SWITCH__
    bool fs = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(w, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
#else
    (void)w;
#endif
}

static void pollInput() {
    in.lookX = in.lookY = 0;
    in.mx = in.my = in.rx = in.ry = 0;
    in.a = in.b = in.x = in.y = in.start = in.run = false;
    in.tap = false;
    in.hover = false;
    for (int i = 0; i < 4; i++) in.dpadPress[i] = false;
    g_capKey = g_capPad = -1;
    g_wheelUp = g_wheelDn = false;
    bool uiOK = false, uiBack = false;
    const bool play = (state == ST_PLAY);

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quitGame();
#ifndef __SWITCH__
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            SDL_Scancode sc = e.key.keysym.scancode;
            if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                toggleFullscreen(SDL_GetWindowFromID(e.key.windowID));
                continue;
            }
            if (g_capKey < 0) g_capKey = sc;
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) uiOK = true;
            if (!play && sc == SDL_SCANCODE_SPACE) uiOK = true;
            if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_BACKSPACE) uiBack = true;
            if (!play) {
                if (sc == SDL_SCANCODE_UP) in.dpadPress[0] = true;
                if (sc == SDL_SCANCODE_DOWN) in.dpadPress[1] = true;
                if (sc == SDL_SCANCODE_LEFT) in.dpadPress[2] = true;
                if (sc == SDL_SCANCODE_RIGHT) in.dpadPress[3] = true;
            }
        }
        if (e.type == SDL_MOUSEMOTION) {
            if (g_mouseCaptured) {
                in.lookX += e.motion.xrel * 0.7f * g_sens;
                in.lookY += e.motion.yrel * 0.7f * g_sens;
            } else {
                toGame(e.motion.x, e.motion.y, in.hoverX, in.hoverY);
                in.hover = true;
                if (g_touchHeld) { g_touchX = in.hoverX; g_touchY = in.hoverY; }
            }
        }
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            if (g_capKey < 0) g_capKey = BK_MOUSE + e.button.button;
            if (!g_mouseCaptured && e.button.button == SDL_BUTTON_LEFT) {
                float x, y; toGame(e.button.x, e.button.y, x, y);
                in.tap = true; in.tapX = x; in.tapY = y;
                g_touchHeld = true; g_touchX = x; g_touchY = y;
            }
        }
        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && !g_mouseCaptured) g_touchHeld = false;
        if (e.type == SDL_MOUSEWHEEL) {
            int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            if (dy > 0) { g_wheelUp = true; if (g_capKey < 0) g_capKey = BK_WHEELUP; }
            if (dy < 0) { g_wheelDn = true; if (g_capKey < 0) g_capKey = BK_WHEELDN; }
        }
#endif
        if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
            e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
            SDL_GameControllerClose(g_pad); g_pad = nullptr;
            for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
        }
        if (e.type == SDL_CONTROLLERAXISMOTION && g_capPad < 0 && e.caxis.value > 16000) {
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) g_capPad = BP_LT;
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) g_capPad = BP_RT;
        }
        // touch (handheld): taps on the UI (TASKS button, menus); the minimap and TASKS
        // are the only touch controls left in play
        if (e.type == SDL_FINGERDOWN) {
            float x = e.tfinger.x * W, y = e.tfinger.y * H;
            in.tap = true; in.tapX = x; in.tapY = y;
            g_touchHeld = true; g_touchX = x; g_touchY = y;
        }
        if (e.type == SDL_FINGERMOTION) { g_touchX = e.tfinger.x * W; g_touchY = e.tfinger.y * H; }
        if (e.type == SDL_FINGERUP && SDL_GetNumTouchFingers(e.tfinger.touchId) <= 0) g_touchHeld = false;
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            if (g_capPad < 0) g_capPad = e.cbutton.button;
            switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: in.dpadPress[0] = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in.dpadPress[1] = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in.dpadPress[2] = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in.dpadPress[3] = true; break;
            }
        }
    }
#ifndef __SWITCH__
    g_mouseBtns = SDL_GetMouseState(nullptr, nullptr);
#endif
    const Uint8* k = SDL_GetKeyboardState(nullptr);

    // movement: bound keys + left stick; menus also take the arrow keys
    if (actDown(A_FWD, k)) in.my -= 1;
    if (actDown(A_BACK, k)) in.my += 1;
    if (actDown(A_LEFT, k)) in.mx -= 1;
    if (actDown(A_RIGHT, k)) in.mx += 1;
    if (g_pad && !g_edCapture) {
        float ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
        float ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
        float bx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
        float by = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
        // radial-ish deadzone rescaled so output starts at 0 instead of jumping to 0.18
        auto dz = [](float v) {
            float a = fabsf(v);
            if (a < 0.15f) return 0.f;
            return (v > 0 ? 1.f : -1.f) * std::min(1.f, (a - 0.15f) / 0.85f);
        };
        in.mx += dz(ax); in.my += dz(ay);
        in.rx += dz(bx); in.ry += dz(by);
    }
    in.mx = clampf(in.mx, -1.f, 1.f); in.my = clampf(in.my, -1.f, 1.f);

    // the UI confirm / back buttons: Enter / Esc and the controller's A / B (by label)
    if (g_pad && !g_edCapture) {
#ifdef __SWITCH__
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B)) uiOK = true;      // labelled A
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A)) uiBack = true;    // labelled B
#else
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A)) uiOK = true;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B)) uiBack = true;
#endif
    }
    if (play) {
        in.a = actDown(A_USE, k); in.b = actDown(A_JUMP, k); in.x = actDown(A_THROW, k); in.run = actDown(A_RUN, k);
    } else if (!g_edCapture) {
        in.a = uiOK; in.b = uiBack;
    }
    in.y = actDown(A_TASKS, k);
    in.start = actDown(A_PAUSE, k);
    in.aPress = in.a && !prevA; in.bPress = in.b && !prevB;
    in.xPress = in.x && !prevX; in.yPress = in.y && !prevY;
    in.startPress = in.start && !prevStart;
    prevA = in.a; prevB = in.b; prevX = in.x; prevY = in.y; prevStart = in.start;
}

// ------------------------------------------------------------------ player
struct Player {
    Vec3 pos;
    Vec3 vel;
    float yaw = 0, pitch = 0;
    bool grounded = false;
    bool jumping = false;
    float airT = 0, lastAir = 0;
    // FirstPersonController step cycle / CurveControlledBob / LerpControlledBob
    float stepCycle = 0, nextStep = 0;
    bool stepAlt = false;
    float bobCX = 0, bobCY = 0, bobX = 0, bobY = 0;
    float landT = -1;
};
static Player P;
static const float FOOT_VOL = 0.6f;

// ------------------------------------------------------------------ level runtime
static const int MAXW = 8;

struct DoorRT { float open = 0; bool locked = false; bool openTarget = false; bool nbInside = false; };
struct PropRT { Vec3 pos; Quat rot; Vec3 vel; bool alive = true; bool held = false; bool rest = false; bool dropped = false;
                Vec3 angVel; float drag = 0.f, angDrag = 0.05f, sleepT = 0.f; };

static struct LevelRun {
    int idx = -1;
    Scene* sc = nullptr;
    std::vector<PropRT> props;
    std::vector<DoorRT> doors;
    std::vector<int> doorProp;
    std::vector<int> lockProp;
    // NeighborAI state (see updateNeighbor)
    int neighbor = -1;                 // >= 0 when the level has an active neighbour
    const CharLevel* nb = nullptr;
    CharAnim nAnim;
    Vec3 nPos, nVel, nDest;
    float nYaw = 0;
    int wpIndex = 0;                   // currentWP
    float wpStay = 0;                  // StayTime coroutine (3 s)
    float nStun = 0;                   // Stunning coroutine (4 s)
    float nCatch = 0;                  // StartCatch coroutine (13 s)
    float nSpeed = 2.f;                // nav.speed
    float nStopDist = 0.f;             // nav.stoppingDistance
    float nStepT = 0;
    bool nAttack = false, nChase = false;
    float deadT = 0;
    bool hasKey = false, hasScissors = false, hasMoney = false, hasGas = false;
    bool carBurned = false, wiresCut = false;
    float fireT = -1;
    bool won = false;
    float wonT = 0;
    int heldIdx = -1;
    bool lockOpened = false;
    // BreakableGlass windows: trigger marker per window, broken flag
    std::vector<int> glassMarker;
    std::vector<char> glassBroken;
    std::string prompt;
} L;

// neighbour NavMeshAgent path (navStep)
static std::vector<Vec3> g_nPath;
static size_t g_nPathI = 0;
static Vec3 g_nPathDest;
static float g_nRepath = 0;


static int g_unlocked = 1;   // levels unlocked (1..7)
static int g_pendingLevel = 0;
static int g_menuSel = 0;
static float g_loadT = 0.f;
static float g_volume = 1.f;   // Menu.Start: PlayerPrefs "Volume" = 1 on first launch
static bool g_mute = false;    // Menu "Mute music" toggle
static bool g_dirtySave = false;

static void saveProgress() {
    const char* path =
#ifdef __SWITCH__
        "sdmc:/switch/darkriddle.sav";
#else
        "darkriddle.sav";
#endif
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int v[3] = { g_unlocked, (int)(g_volume * 100 + 0.5f), 0x100 | (g_ctrlType & 1) | (g_mute ? 2 : 0) };
    fwrite(v, sizeof(int), 3, f);
    fclose(f);
}

static void loadProgress() {
    const char* path =
#ifdef __SWITCH__
        "sdmc:/switch/darkriddle.sav";
#else
        "darkriddle.sav";
#endif
    FILE* f = fopen(path, "rb");
    if (!f) return;
    int v[3] = { 1, 80, 1 };
    if (fread(v, sizeof(int), 3, f) == 3) {
        g_unlocked = std::max(1, std::min(7, v[0]));
        g_volume = clampf(v[1] / 100.f, 0, 1);
        if (v[2] & 0x100) { g_ctrlType = v[2] & 1; g_mute = (v[2] & 2) != 0; }
    }
    fclose(f);
}

// ------------------------------------------------------------------ collision
static bool aabbOverlap(const Vec3& amn, const Vec3& amx, const Vec3& bmn, const Vec3& bmx) {
    return amn.x < bmx.x && amx.x > bmn.x && amn.y < bmx.y && amx.y > bmn.y && amn.z < bmx.z && amx.z > bmn.z;
}

// Walkable-surface height grid built from the level's own world-baked geometry.
struct HeightGrid {
    float x0 = 0, z0 = 0, cell = 2.f;
    int nx = 0, nz = 0;
    std::vector<float> h;   // highest surface below each cell's ceiling
    std::vector<float> top; // absolute highest surface in the cell

    void build(const Scene* s) {
        float mnx = 1e9f, mnz = 1e9f, mxx = -1e9f, mxz = -1e9f;
        for (int mid : s->meshIds) {
            Mesh* m = getMesh(mid);
            if (!m) continue;
            size_t nv = m->verts.size() / 24;
            for (size_t i = 0; i < nv; i += 3) {
                float vx, vz;
                memcpy(&vx, &m->verts[i * 24 + 0], 4);
                memcpy(&vz, &m->verts[i * 24 + 8], 4);
                if (vx < mnx) mnx = vx; if (vx > mxx) mxx = vx;
                if (vz < mnz) mnz = vz; if (vz > mxz) mxz = vz;
            }
        }
        if (mnx > mxx) { nx = nz = 0; return; }
        x0 = mnx; z0 = mnz;
        nx = (int)((mxx - mnx) / cell) + 1;
        nz = (int)((mxz - mnz) / cell) + 1;
        h.assign((size_t)nx * nz, 1e9f);
        top.assign((size_t)nx * nz, 1e9f);
        for (int mid : s->meshIds) {
            Mesh* m = getMesh(mid);
            if (!m) continue;
            size_t nv = m->verts.size() / 24;
            for (size_t i = 0; i < nv; i += 2) {
                float vx, vy, vz;
                memcpy(&vx, &m->verts[i * 24 + 0], 4);
                memcpy(&vy, &m->verts[i * 24 + 4], 4);
                memcpy(&vz, &m->verts[i * 24 + 8], 4);
                int cx = (int)((vx - x0) / cell), cz = (int)((vz - z0) / cell);
                if (cx < 0 || cz < 0 || cx >= nx || cz >= nz) continue;
                float& t = top[(size_t)cz * nx + cx];
                if (vy < t) t = vy;   // lowest surface in the cell approximates the floor
            }
        }
        // smoke: fill from neighbours so tiny gaps still give a surface
        for (int pass = 0; pass < 2; pass++) {
            std::vector<float> src = top;
            for (int z = 1; z < nz - 1; z++) for (int x = 1; x < nx - 1; x++) {
                float& t = top[(size_t)z * nx + x];
                if (t < 1e8f) continue;
                float best = 1e9f;
                for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
                    float v = src[(size_t)(z + dz) * nx + (x + dx)];
                    if (v < best) best = v;
                }
                if (best < 1e8f) t = best;
            }
        }
    }

    float at(float x, float z, float belowY) const {
        if (nx <= 0) return -1e9f;
        int cx = (int)((x - x0) / cell), cz = (int)((z - z0) / cell);
        if (cx < 0 || cz < 0 || cx >= nx || cz >= nz) return -1e9f;
        return top[(size_t)cz * nx + cx];
    }
};

static HeightGrid g_hf;
static float g_groundY = 0.f;

// Triangle grid over the level's rendered geometry: real collision with walls,
// floors and terrain instead of the approximate box colliders from Unity.
struct TriGrid {
    float x0 = 0, z0 = 0, cell = 4.f;
    int nx = 0, nz = 0;
    std::vector<std::vector<unsigned int> > cells;
    std::vector<float> tri;   // 9 floats per triangle

    void build(const Scene* s) {
        float mnx = 1e9f, mnz = 1e9f, mxx = -1e9f, mxz = -1e9f;
        tri.clear();
        // Unity's colliders (type-12 collision mesh) when the level has them, else the
        // render geometry
        std::vector<int> ids;
        for (const Prop& p : s->props) if (p.type == 12) ids.push_back(p.meshId);
        if (ids.empty()) ids = s->meshIds;
        for (int mid : ids) {
            Mesh* m = getMesh(mid);
            if (!m) continue;
            size_t nv = m->verts.size() / 24;
            for (size_t i = 0; i < nv; i++) {
                float vx, vz;
                memcpy(&vx, &m->verts[i * 24 + 0], 4);
                memcpy(&vz, &m->verts[i * 24 + 8], 4);
                if (vx < mnx) mnx = vx; if (vx > mxx) mxx = vx;
                if (vz < mnz) mnz = vz; if (vz > mxz) mxz = vz;
            }
        }
        if (mnx > mxx) { nx = nz = 0; return; }
        x0 = mnx; z0 = mnz;
        nx = (int)((mxx - mnx) / cell) + 1;
        nz = (int)((mxz - mnz) / cell) + 1;
        cells.assign((size_t)nx * nz, std::vector<unsigned int>());
        for (int mid : ids) {
            Mesh* m = getMesh(mid);
            if (!m) continue;
            size_t nv = m->verts.size() / 24;
            for (size_t i = 0; i + 2 < m->idx.size(); i += 3) {
                unsigned int a = m->idx[i], b = m->idx[i + 1], c = m->idx[i + 2];
                if (a >= nv || b >= nv || c >= nv) continue;
                float va[3], vb[3], vc[3];
                memcpy(va, &m->verts[a * 24], 12);
                memcpy(vb, &m->verts[b * 24], 12);
                memcpy(vc, &m->verts[c * 24], 12);
                // register the triangle in every cell its XZ bounds overlap (large
                // ground quads span many cells)
                float lox = std::min(va[0], std::min(vb[0], vc[0])), hix = std::max(va[0], std::max(vb[0], vc[0]));
                float loz = std::min(va[2], std::min(vb[2], vc[2])), hiz = std::max(va[2], std::max(vb[2], vc[2]));
                int gx0 = std::max(0, (int)((lox - x0) / cell)), gx1 = std::min(nx - 1, (int)((hix - x0) / cell));
                int gz0 = std::max(0, (int)((loz - z0) / cell)), gz1 = std::min(nz - 1, (int)((hiz - z0) / cell));
                if (gx0 > gx1 || gz0 > gz1) continue;
                unsigned int ti = (unsigned int)(tri.size() / 9);
                tri.insert(tri.end(), va, va + 3);
                tri.insert(tri.end(), vb, vb + 3);
                tri.insert(tri.end(), vc, vc + 3);
                for (int gz = gz0; gz <= gz1; gz++)
                    for (int gx = gx0; gx <= gx1; gx++) cells[(size_t)gz * nx + gx].push_back(ti);
            }
        }
    }

    static bool hitTri(const float* p0, const float* p1, const float* p2, const Vec3& o, const Vec3& d, float maxT, float& tOut) {
        Vec3 e1(p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]);
        Vec3 e2(p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]);
        Vec3 pv(d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x);
        float det = e1.x * pv.x + e1.y * pv.y + e1.z * pv.z;
        if (det > -1e-6f && det < 1e-6f) return false;
        float inv = 1.f / det;
        Vec3 tv(o.x - p0[0], o.y - p0[1], o.z - p0[2]);
        float u = (tv.x * pv.x + tv.y * pv.y + tv.z * pv.z) * inv;
        if (u < 0.f || u > 1.f) return false;
        Vec3 qv(tv.y * e1.z - tv.z * e1.y, tv.z * e1.x - tv.x * e1.z, tv.x * e1.y - tv.y * e1.x);
        float v = (d.x * qv.x + d.y * qv.y + d.z * qv.z) * inv;
        if (v < 0.f || u + v > 1.f) return false;
        float t = (e2.x * qv.x + e2.y * qv.y + e2.z * qv.z) * inv;
        if (t < 0.001f || t > maxT) return false;
        tOut = t;
        return true;
    }

    // closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5)
    static Vec3 closestOnTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
        Vec3 ab = b - a, ac = c - a, ap = p - a;
        float d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) return a;
        Vec3 bp = p - b; float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) return b;
        float vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
        Vec3 cp = p - c; float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) return c;
        float vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
        float va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        float den = 1.f / (va + vb + vc);
        return a + ab * (vb * den) + ac * (vc * den);
    }

    // horizontal push that moves a sphere (centre c, radius r) out of every steep
    // triangle (walls; floors and stair treads are handled by the ground rays)
    Vec3 pushOut(const Vec3& c, float r) const {
        Vec3 push;
        if (nx <= 0) return push;
        int ax = std::max(0, (int)((c.x - r - x0) / cell)), bx = std::min(nx - 1, (int)((c.x + r - x0) / cell));
        int az = std::max(0, (int)((c.z - r - z0) / cell)), bz = std::min(nz - 1, (int)((c.z + r - z0) / cell));
        for (int gz = az; gz <= bz; gz++) for (int gx = ax; gx <= bx; gx++) {
            const std::vector<unsigned int>& list = cells[(size_t)gz * nx + gx];
            for (size_t k = 0; k < list.size(); k++) {
                const float* t = &tri[(size_t)list[k] * 9];
                Vec3 a(t[0], t[1], t[2]), b(t[3], t[4], t[5]), cc(t[6], t[7], t[8]);
                float lo = std::min(a.y, std::min(b.y, cc.y)), hi = std::max(a.y, std::max(b.y, cc.y));
                if (c.y + r < lo || c.y - r > hi) continue;
                Vec3 n = cross(b - a, cc - a);
                float nl = n.len();
                if (nl < 1e-8f || fabsf(n.y / nl) > 0.7f) continue;   // walkable surface
                Vec3 q = closestOnTri(c + push, a, b, cc);
                Vec3 d = (c + push) - q;
                float dl = d.len();                       // true sphere overlap
                if (dl >= r) continue;
                Vec3 hd(d.x, 0, d.z);                     // but only pushed sideways
                float hl = hd.len();
                if (hl < 1e-5f) { hd = Vec3(n.x, 0, n.z); hl = hd.len(); if (hl < 1e-6f) continue; }
                push += hd * ((r - dl) / hl);
            }
        }
        return push;
    }

    // raycast that also returns the hit triangle's unit normal
    bool raycastN(const Vec3& o, const Vec3& d, float maxT, float& tOut, Vec3& nOut) const {
        if (nx <= 0) return false;
        float ex = o.x + d.x * maxT, ez = o.z + d.z * maxT;
        int ax = std::max(0, (int)((std::min(o.x, ex) - x0) / cell)), bx = std::min(nx - 1, (int)((std::max(o.x, ex) - x0) / cell));
        int az = std::max(0, (int)((std::min(o.z, ez) - z0) / cell)), bz = std::min(nz - 1, (int)((std::max(o.z, ez) - z0) / cell));
        bool found = false;
        tOut = maxT;
        for (int gz = az; gz <= bz; gz++) for (int gx = ax; gx <= bx; gx++) {
            const std::vector<unsigned int>& list = cells[(size_t)gz * nx + gx];
            for (size_t k = 0; k < list.size(); k++) {
                const float* p = &tri[(size_t)list[k] * 9];
                float t;
                if (hitTri(p, p + 3, p + 6, o, d, tOut, t) && t < tOut) {
                    tOut = t; found = true;
                    Vec3 n = cross(Vec3(p[3] - p[0], p[4] - p[1], p[5] - p[2]), Vec3(p[6] - p[0], p[7] - p[1], p[8] - p[2]));
                    float l = n.len();
                    nOut = l > 1e-9f ? n * (1.f / l) : Vec3(0, 1, 0);
                }
            }
        }
        return found;
    }

    // every triangle closer than r to c: contact point, normal (towards c), depth
    template <class F> void sphereContacts(const Vec3& c, float r, F&& add) const {
        if (nx <= 0) return;
        int ax = std::max(0, (int)((c.x - r - x0) / cell)), bx = std::min(nx - 1, (int)((c.x + r - x0) / cell));
        int az = std::max(0, (int)((c.z - r - z0) / cell)), bz = std::min(nz - 1, (int)((c.z + r - z0) / cell));
        for (int gz = az; gz <= bz; gz++) for (int gx = ax; gx <= bx; gx++) {
            const std::vector<unsigned int>& list = cells[(size_t)gz * nx + gx];
            for (size_t k = 0; k < list.size(); k++) {
                const float* t = &tri[(size_t)list[k] * 9];
                Vec3 q = closestOnTri(c, Vec3(t[0], t[1], t[2]), Vec3(t[3], t[4], t[5]), Vec3(t[6], t[7], t[8]));
                Vec3 d = c - q;
                float dl = d.len();
                if (dl >= r || dl < 1e-6f) continue;
                add(q, d * (1.f / dl), r - dl);
            }
        }
    }

    bool raycast(const Vec3& o, const Vec3& d, float maxT, float& tOut) const {
        if (nx <= 0) return false;
        float ex = o.x + d.x * maxT, ez = o.z + d.z * maxT;
        int ax = (int)((std::min(o.x, ex) - x0) / cell), bx = (int)((std::max(o.x, ex) - x0) / cell);
        int az = (int)((std::min(o.z, ez) - z0) / cell), bz = (int)((std::max(o.z, ez) - z0) / cell);
        ax = std::max(ax, 0); az = std::max(az, 0);
        bx = std::min(bx, nx - 1); bz = std::min(bz, nz - 1);
        bool found = false;
        tOut = maxT;
        for (int gz = az; gz <= bz; gz++) for (int gx = ax; gx <= bx; gx++) {
            const std::vector<unsigned int>& list = cells[(size_t)gz * nx + gx];
            for (size_t k = 0; k < list.size(); k++) {
                const float* p = &tri[(size_t)list[k] * 9];
                float t;
                if (hitTri(p, p + 3, p + 6, o, d, tOut, t) && t < tOut) { tOut = t; found = true; }
            }
        }
        return found;
    }
};

static TriGrid g_tg;

static void playerBox(Vec3& mn, Vec3& mx, const Vec3& pos, float radius, float height) {
    mn = Vec3(pos.x - radius, pos.y, pos.z - radius);
    mx = Vec3(pos.x + radius, pos.y + height, pos.z + radius);
}

static void resolveAxis(Vec3& pos, Vec3& vel, float radius, float height, int axis, float sign, bool& grounded) {
    Vec3 mn, mx;
    playerBox(mn, mx, pos, radius, height);
    for (auto& c : L.sc->colliders) {
        if ((c.mx.x - c.mn.x) > 60.f || (c.mx.y - c.mn.y) > 60.f || (c.mx.z - c.mn.z) > 60.f) continue;
        if (!aabbOverlap(mn, mx, c.mn, c.mx)) continue;
        if (axis == 0) {
            if (sign > 0) pos.x = c.mn.x - radius - 0.001f; else pos.x = c.mx.x + radius + 0.001f;
            vel.x = 0;
        } else if (axis == 2) {
            if (sign > 0) pos.z = c.mn.z - radius - 0.001f; else pos.z = c.mx.z + radius + 0.001f;
            vel.z = 0;
        } else {
            if (sign > 0) { pos.y = c.mn.y - height - 0.001f; vel.y = 0; }
            else { pos.y = c.mx.y + 0.001f; vel.y = 0; grounded = true; }
        }
        playerBox(mn, mx, pos, radius, height);
    }
}

static bool moveCapsule(Vec3& pos, Vec3& vel, float radius, float height, float dt, bool& grounded) {
    Vec3 step = vel * dt;
    grounded = false;
    float midY = pos.y + height * 0.5f;
    float t = 0.f;

    // horizontal movement: probe ahead against the level triangles
    if (step.x != 0.f) {
        float dir = step.x > 0 ? 1.f : -1.f;
        if (g_tg.raycast(Vec3(pos.x, midY, pos.z), Vec3(dir, 0, 0), fabsf(step.x) + radius, t))
            { pos.x += dir * std::max(0.f, t - radius); vel.x = 0.f; }
        else pos.x += step.x;
    }
    if (step.z != 0.f) {
        float dir = step.z > 0 ? 1.f : -1.f;
        if (g_tg.raycast(Vec3(pos.x, midY, pos.z), Vec3(0, 0, dir), fabsf(step.z) + radius, t))
            { pos.z += dir * std::max(0.f, t - radius); vel.z = 0.f; }
        else pos.z += step.z;
    }

    // vertical: floor and ceiling probes
    if (step.y < 0.f) {
        if (g_tg.raycast(Vec3(pos.x, pos.y + 0.6f, pos.z), Vec3(0, -1, 0), 0.6f - step.y + 0.05f, t)) {
            pos.y = pos.y + 0.6f - t;
            vel.y = 0.f;
            grounded = true;
        } else pos.y += step.y;
    } else if (step.y > 0.f) {
        if (g_tg.raycast(Vec3(pos.x, pos.y + height - 0.1f, pos.z), Vec3(0, 1, 0), step.y + 0.1f, t)) {
            pos.y += std::max(0.f, t - 0.1f);
            vel.y = 0.f;
        } else pos.y += step.y;
    } else {
        if (g_tg.raycast(Vec3(pos.x, pos.y + 0.35f, pos.z), Vec3(0, -1, 0), 0.5f, t)) {
            pos.y = pos.y + 0.35f - t;
            vel.y = 0.f;
            grounded = true;
        }
    }
    // safety net under the level's own ground geometry
    if (pos.y <= g_groundY) { pos.y = g_groundY; vel.y = 0.f; grounded = true; }
    return grounded;
}

static bool rayAABB(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, float& tmin) {
    float t1 = (mn.x - o.x) / (d.x == 0 ? 1e-9f : d.x);
    float t2 = (mx.x - o.x) / (d.x == 0 ? 1e-9f : d.x);
    float t3 = (mn.y - o.y) / (d.y == 0 ? 1e-9f : d.y);
    float t4 = (mx.y - o.y) / (d.y == 0 ? 1e-9f : d.y);
    float t5 = (mn.z - o.z) / (d.z == 0 ? 1e-9f : d.z);
    float t6 = (mx.z - o.z) / (d.z == 0 ? 1e-9f : d.z);
    float tmax = std::min(std::min(std::max(t1, t2), std::max(t3, t4)), std::max(t5, t6));
    tmin = std::max(std::max(std::min(t1, t2), std::min(t3, t4)), std::min(t5, t6));
    return tmax > std::max(tmin, 0.f);
}

static bool raycastStatic(const Vec3& o, const Vec3& d, float maxT, float& hitT) {
    // the level's own triangles (box colliders from concave MeshColliders would
    // cover whole rooms)
    hitT = maxT;
    float t;
    if (maxT > 0.f && g_tg.raycast(o, d, maxT, t)) { hitT = t; return true; }
    return false;
}

// ------------------------------------------------------------------ helpers
static Vec3 camForward() {
    return Vec3(sinf(P.yaw) * cosf(P.pitch), sinf(P.pitch), cosf(P.yaw) * cosf(P.pitch));
}
static Vec3 camRight() { return Vec3(cosf(P.yaw), 0, -sinf(P.yaw)); }
// FPSController: CharacterController height 1.8 centred on the transform, resting on its
// 0.08 skin; FirstPersonCharacter (the camera) 1.087 above the transform:
// eye = feet + 0.08 + 0.9 + 1.087
static const float EYE_H = 2.067f;
static Vec3 eyePos() { return Vec3(P.pos.x, P.pos.y + EYE_H, P.pos.z); }

// the player's CharacterController (r 0.34, h 1.75) touching a trigger box marker
static bool inTrigger(const Marker& m, float margin) {
    const float r = 0.34f + margin;
    return P.pos.x + r > m.pos.x - m.half.x && P.pos.x - r < m.pos.x + m.half.x &&
           P.pos.z + r > m.pos.z - m.half.z && P.pos.z - r < m.pos.z + m.half.z &&
           P.pos.y + 1.75f + margin > m.pos.y - m.half.y && P.pos.y - margin < m.pos.y + m.half.y;
}

static float dist2D(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

static Marker* findMarker(int type) {
    for (auto& m : L.sc->markers) if (m.type == type) return &m;
    return nullptr;
}

static float g_promptT = 0.f;
static void toast(const char* msg, float t = 2.5f) { L.prompt = msg; g_promptT = t; }
static void modelForProp(Prop& p, PropRT& r, Mat4& out, bool doorOpen, float doorT);

// BreakableGlass.OnTriggerEnter: solidGlass off, brokenGlass on (its rigidbody shards fall)
static void breakGlass(int k) {
    if (k < 0 || k >= (int)L.glassBroken.size() || L.glassBroken[k]) return;
    L.glassBroken[k] = 1;
    char nm[16];
    snprintf(nm, sizeof nm, "G%d", k);
    for (size_t i = 0; i < L.sc->props.size(); i++) {
        Prop& p = L.sc->props[i];
        if ((p.type != 8 && p.type != 9) || strcmp(p.name, nm) != 0) continue;
        PropRT& r = L.props[i];
        if (p.type == 8) { r.alive = false; continue; }
        r.alive = true; r.dropped = true; r.rest = false;
        r.vel = Vec3(((rand() % 100) - 50) * 0.01f, (rand() % 100) * 0.01f, ((rand() % 100) - 50) * 0.01f);
    }
    int m = L.glassMarker[k];
    float d = m >= 0 ? (L.sc->markers[m].pos - P.pos).len() : 0.f;
    (void)d;
    audioPlay("2000038371_306055549 (mp3cut.ru)", 1.f);   // BreakableGlass AudioSource (2D)
}

// ------------------------------------------------------------------ rigid bodies
// Unity Rigidbody (mass 1, default PhysicMaterial: friction 0.6, bounciness 0) for the
// "Take" objects, the dropped lock and the glass shards: box colliders from the mesh
// bounds (Ball_1: sphere), against the level's colliders.
static Quat qmul(const Quat& a, const Quat& b) {
    return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat qnorm(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-8f) return Quat();
    q.x /= n; q.y /= n; q.z /= n; q.w /= n; return q;
}
static Vec3 qrot(const Quat& q, const Vec3& v) {
    Mat4 m = matFromQuat(q);
    return Vec3(m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z, m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
                m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z);
}
static Vec3 qrotInv(const Quat& q, const Vec3& v) { return qrot(Quat{-q.x, -q.y, -q.z, q.w}, v); }

struct PhysContact { Vec3 p, n; float depth; };

static void stepBody(Prop& p, PropRT& r, float dt) {
    const float mu = 0.6f, invM = 1.f;
    bool sphere = strncmp(p.name, "Ball", 4) == 0;
    Vec3 h(std::max(0.02f, fabsf(p.half.x)), std::max(0.02f, fabsf(p.half.y)), std::max(0.02f, fabsf(p.half.z)));
    float rad = std::max(h.x, std::max(h.y, h.z));
    // body-frame inverse inertia (box / solid sphere, mass 1)
    Vec3 ib = sphere ? Vec3(1, 1, 1) * (1.f / (0.4f * rad * rad))
                     : Vec3(3.f / (h.y * h.y + h.z * h.z), 3.f / (h.x * h.x + h.z * h.z), 3.f / (h.x * h.x + h.y * h.y));
    auto Iinv = [&](const Vec3& v) { Vec3 b = qrotInv(r.rot, v); return qrot(r.rot, Vec3(b.x * ib.x, b.y * ib.y, b.z * ib.z)); };

    Vec3 com = r.pos + qrot(r.rot, p.center);
    r.vel.y -= 9.81f * dt;
    r.vel = r.vel * (1.f / (1.f + r.drag * dt));
    r.angVel = r.angVel * (1.f / (1.f + r.angDrag * dt));
    com += r.vel * dt;
    Quat wq{r.angVel.x, r.angVel.y, r.angVel.z, 0.f};
    Quat dq = qmul(wq, r.rot);
    r.rot = qnorm(Quat{r.rot.x + dq.x * 0.5f * dt, r.rot.y + dq.y * 0.5f * dt, r.rot.z + dq.z * 0.5f * dt, r.rot.w + dq.w * 0.5f * dt});

    PhysContact cs[24]; int nc = 0;
    if (sphere) {
        g_tg.sphereContacts(com, rad, [&](const Vec3& q, const Vec3& n, float d) { if (nc < 24) cs[nc++] = {q, n, d}; });
    } else {
        Vec3 pts[14]; int np = 0;
        for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
            pts[np++] = Vec3(h.x * sx, h.y * sy, h.z * sz);
        pts[np++] = Vec3(h.x, 0, 0); pts[np++] = Vec3(-h.x, 0, 0); pts[np++] = Vec3(0, h.y, 0);
        pts[np++] = Vec3(0, -h.y, 0); pts[np++] = Vec3(0, 0, h.z); pts[np++] = Vec3(0, 0, -h.z);
        for (int k = 0; k < np && nc < 24; k++) {
            Vec3 d = qrot(r.rot, pts[k]);
            float len = d.len();
            if (len < 1e-4f) continue;
            Vec3 dir = d * (1.f / len), n;
            float t;
            if (!g_tg.raycastN(com, dir, len, t, n)) continue;
            if (dot(n, dir) > 0) n = n * -1.f;
            cs[nc++] = {com + dir * t, n, (len - t) * fabsf(dot(dir, n))};
        }
    }
    // sequential impulses: no restitution (bounciness 0), Coulomb friction 0.6
    for (int it = 0; it < 6; it++) {
        for (int k = 0; k < nc; k++) {
            Vec3 rr = cs[k].p - com, n = cs[k].n;
            Vec3 vrel = r.vel + cross(r.angVel, rr);
            float vn = dot(vrel, n);
            if (vn >= 0.f) continue;
            Vec3 rn = cross(rr, n);
            float kn = invM + dot(n, cross(Iinv(rn), rr));
            float jn = -vn / kn;
            r.vel += n * (jn * invM);
            r.angVel += Iinv(rn * jn);
            vrel = r.vel + cross(r.angVel, rr);
            Vec3 vt = vrel - n * dot(vrel, n);
            float vtl = vt.len();
            if (vtl > 1e-5f) {
                Vec3 tdir = vt * (1.f / vtl);
                Vec3 rt = cross(rr, tdir);
                float kt = invM + dot(tdir, cross(Iinv(rt), rr));
                float jt = std::min(vtl / kt, mu * jn);
                r.vel += tdir * (-jt * invM);
                r.angVel += Iinv(rt * -jt);
            }
        }
    }
    // push out of the deepest penetration
    int deep = -1;
    for (int k = 0; k < nc; k++) if (deep < 0 || cs[k].depth > cs[deep].depth) deep = k;
    if (deep >= 0 && cs[deep].depth > 0.002f) com += cs[deep].n * (cs[deep].depth - 0.002f);
    if (com.y < g_groundY) { com.y = g_groundY; if (r.vel.y < 0) r.vel.y = 0; }
    r.pos = com - qrot(r.rot, p.center);
    // Rigidbody sleep
    if (nc > 0 && r.vel.len() < 0.08f && r.angVel.len() < 0.15f) {
        r.sleepT += dt;
        if (r.sleepT > 0.4f) { r.vel = Vec3(); r.angVel = Vec3(); r.rest = true; }
    } else r.sleepT = 0.f;
}

// a window's trigger box (intact windows also block like the GlassCinema collider)
static bool glassBox(int k, Vec3& mn, Vec3& mx) {
    int m = (k >= 0 && k < (int)L.glassMarker.size()) ? L.glassMarker[k] : -1;
    if (m < 0) return false;
    const Marker& mk = L.sc->markers[m];
    mn = mk.pos - mk.half; mx = mk.pos + mk.half;
    return true;
}
static void applyHudTaps();

// ------------------------------------------------------------------ level start
// Sound.PlayMusicSound / PlayCatchSound: assign the clip, play it if not playing
static std::string g_music;
static void stopMusic() { audioStopMusic(); g_music.clear(); }
static void setMusic(const char* name) {
    if (g_music == name) return;
    stopMusic();
    audioPlay(name, g_volume, true);
    g_music = name;
}

static void startLevel(int idx) {
    if (idx < 0 || idx >= (int)g_scenes.size()) return;
    L = LevelRun();
    L.idx = idx;
    L.sc = &g_scenes[idx];
    g_hf.build(L.sc);
    g_tg.build(L.sc);

    // player spawn
    Marker* sp = findMarker(1);
    P = Player();
    P.pos = sp ? sp->pos : L.sc->spawn;
    // safety floor just below the real ground under the spawn point
    {
        float t;
        Vec3 o(P.pos.x, P.pos.y + 1.f, P.pos.z);
        if (g_tg.raycast(o, Vec3(0, -1, 0), 60.f, t)) { P.pos.y = o.y - t; g_groundY = P.pos.y - 0.25f; }
        else g_groundY = P.pos.y - 2.f;
    }
    P.vel = Vec3();
    P.yaw = 0;
    // face the house (towards the neighbour spawn)
    Marker* nb = findMarker(2);
    if (nb) {
        Vec3 d = nb->pos - P.pos;
        P.yaw = atan2f(d.x, d.z);
    }
    P.pitch = 0;

    L.props.resize(L.sc->props.size());
    L.doors.clear();
    for (size_t i = 0; i < L.sc->props.size(); i++) {
        Prop& p = L.sc->props[i];
        L.props[i].pos = p.pos;
        L.props[i].rot = p.rot;
        L.props[i].vel = Vec3();
        L.props[i].alive = true;
        // Unity rigidbodies start awake: anything placed in mid-air falls and settles
        L.props[i].rest = (p.type != 2);
        L.props[i].dropped = false;
        L.props[i].angVel = Vec3(); L.props[i].sleepT = 0.f;
        // Rigidbody defaults of the scene objects (Ball_1: drag 2, angular drag 0.15)
        bool ball = strncmp(p.name, "Ball", 4) == 0;
        L.props[i].drag = ball ? 2.f : 0.f; L.props[i].angDrag = ball ? 0.15f : 0.05f;
        if (p.type == 1) {
            DoorRT d;
            L.doors.push_back(d);
            L.doorProp.push_back((int)i);
        }
    }
    // locks: a lock belongs to the nearest door
    L.lockProp.assign(L.sc->props.size(), 0);
    for (size_t i = 0; i < L.sc->props.size(); i++) {
        if (L.sc->props[i].type != 4) continue;
        float best = 4.f;
        int bestDoor = -1;
        for (size_t d = 0; d < L.doors.size(); d++) {
            float dd = dist2D(L.sc->props[L.doorProp[d]].pos, L.sc->props[i].pos);
            if (dd < best) { best = dd; bestDoor = (int)d; }
        }
        if (bestDoor >= 0) L.doors[bestDoor].locked = true;
    }
    // BreakableGlass.Start: brokenGlass.SetActive(false)
    L.glassMarker.clear();
    for (size_t m = 0; m < L.sc->markers.size(); m++) {
        const Marker& mk = L.sc->markers[m];
        int k = -1;
        if (mk.type == 10 && sscanf(mk.name, "G%d", &k) == 1 && k >= 0) {
            if ((int)L.glassMarker.size() <= k) L.glassMarker.resize(k + 1, -1);
            L.glassMarker[k] = (int)m;
        }
    }
    L.glassBroken.assign(L.glassMarker.size(), 0);
    for (size_t i = 0; i < L.sc->props.size(); i++)
        if (L.sc->props[i].type == 9) L.props[i].alive = false;

    // the neighbour: NeighborNew's start pose and NeighborAI settings of this level
    L.nb = charLevel(L.sc->level);
    if (L.nb && L.nb->present && L.nb->active) {
        L.neighbor = 0;
        L.nPos = L.nb->pos;
        L.nYaw = L.nb->yaw;
    } else if (nb) {
        L.nPos = nb->pos;
    }
    L.nDest = L.nPos;
    L.nVel = Vec3();
    L.wpIndex = 0;
    L.nAnim = CharAnim();
    g_nPath.clear();

    // markers present in this level
    L.hasKey = L.hasScissors = L.hasMoney = L.hasGas = false;
    L.carBurned = false; L.wiresCut = false; L.won = false; L.heldIdx = -1;
    L.fireT = -1; L.nStun = 0; L.nChase = false; L.lockOpened = false;
    L.nCatch = 0; L.wpStay = 0; L.nAttack = false; L.nSpeed = 2.f; L.nStopDist = 0.f;
    L.prompt.clear();

    // the original opens every level with the StartTasksPanel story parchment
    state = ST_INTRO;
    stateT = 0;
    // Sound.mainMusic ("MENU") is what the player's AudioSource plays while the
    // neighbour is not chasing; Sound.catchMusic ("Загрузка") during a chase
    setMusic("MENU");
}

static const char* taskText(int level) {
    switch (level) {
        case 1: return "Find the key and open the lock";
        case 2: return "Cut the wires with the scissors";
        case 3: return "Steal the money and get back home";
        case 4: return "Burn the car with the gas can";
        case 5: return "Steal the money and return home";
        case 6: return "Get in and open the lock";
        case 7: return "Cut the wires";
    }
    return "";
}

// ------------------------------------------------------------------ interact
static bool closestOnProp(size_t i, const Vec3& wp, Vec3& out) {
    Prop& p = L.sc->props[i];
    PropRT& r = L.props[i];
    if (!r.alive) return false;
    float doorT = 0.f;
    if (p.type == 1)
        for (size_t k = 0; k < L.doorProp.size(); k++) if (L.doorProp[k] == (int)i) doorT = L.doors[k].open;
    Mat4 M;
    modelForProp(p, r, M, p.type == 1, doorT);
    const float* m = M.m;
    // columns are orthonormal here (props are baked with rotation only + pivot)
    Vec3 cx(m[0], m[1], m[2]), cy(m[4], m[5], m[6]), cz(m[8], m[9], m[10]), t(m[12], m[13], m[14]);
    Vec3 d = wp - t;
    Vec3 l(dot(d, cx), dot(d, cy), dot(d, cz));
    Vec3 mn = p.center - p.half, mx = p.center + p.half;
    l.x = clampf(l.x, mn.x, mx.x); l.y = clampf(l.y, mn.y, mx.y); l.z = clampf(l.z, mn.z, mx.z);
    out = t + cx * l.x + cy * l.y + cz * l.z;
    return true;
}

// ray against a prop's oriented mesh box (its world model matrix, door swing included)
static bool rayHitsProp(size_t i, const Vec3& o, const Vec3& d, float& tHit) {
    Prop& p = L.sc->props[i];
    PropRT& r = L.props[i];
    float doorT = 0.f;
    if (p.type == 1)
        for (size_t k = 0; k < L.doorProp.size(); k++) if (L.doorProp[k] == (int)i) doorT = L.doors[k].open;
    Mat4 M;
    modelForProp(p, r, M, p.type == 1, doorT);
    // inverse of the 3x3 part (column-major m[0..2], m[4..6], m[8..10])
    const float* m = M.m;
    float a00 = m[0], a10 = m[1], a20 = m[2], a01 = m[4], a11 = m[5], a21 = m[6], a02 = m[8], a12 = m[9], a22 = m[10];
    float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    if (fabsf(det) < 1e-12f) return false;
    float id = 1.f / det;
    float i00 = (a11 * a22 - a12 * a21) * id, i01 = (a02 * a21 - a01 * a22) * id, i02 = (a01 * a12 - a02 * a11) * id;
    float i10 = (a12 * a20 - a10 * a22) * id, i11 = (a00 * a22 - a02 * a20) * id, i12 = (a02 * a10 - a00 * a12) * id;
    float i20 = (a10 * a21 - a11 * a20) * id, i21 = (a01 * a20 - a00 * a21) * id, i22 = (a00 * a11 - a01 * a10) * id;
    Vec3 q(o.x - m[12], o.y - m[13], o.z - m[14]);
    Vec3 lo(i00 * q.x + i01 * q.y + i02 * q.z, i10 * q.x + i11 * q.y + i12 * q.z, i20 * q.x + i21 * q.y + i22 * q.z);
    Vec3 ld(i00 * d.x + i01 * d.y + i02 * d.z, i10 * d.x + i11 * d.y + i12 * d.z, i20 * d.x + i21 * d.y + i22 * d.z);
    // a little slack so thin objects (locks, keys) are not pixel hunts on a stick
    Vec3 pad(0.04f / std::max(1e-3f, fabsf(p.scale.x)), 0.04f / std::max(1e-3f, fabsf(p.scale.y)), 0.04f / std::max(1e-3f, fabsf(p.scale.z)));
    Vec3 mn = p.center - p.half - pad, mx = p.center + p.half + pad;
    return rayAABB(lo, ld, mn, mx, tHit);   // t is in world units: ld = A^-1 d keeps the parameter
}

// FirstPersonController.Update: Physics.Raycast from the screen centre; "Take" within
// 2 m (only with empty hands), "Door" within 2.5 m (button only when the door's lock
// is open), "Lock" within 2.5 m (button only with the key)
static int findInteractable() {
    Vec3 eye = eyePos();
    Vec3 fwd = camForward();
    float bestT = 1e9f;
    int best = -1;
    for (size_t i = 0; i < L.sc->props.size(); i++) {
        Prop& p = L.sc->props[i];
        PropRT& r = L.props[i];
        if (!r.alive || r.held || r.dropped) continue;
        float maxT;
        if (p.type == 2) { if (L.heldIdx >= 0) continue; maxT = 2.f; }
        else if (p.type == 1 || p.type == 4) maxT = 2.5f;
        else continue;
        if ((L.props[i].pos - eye).len() > maxT + 3.f) continue;
        float t;
        if (!rayHitsProp(i, eye, fwd, t) || t > maxT || t >= bestT) continue;
        float rt;
        if (raycastStatic(eye, fwd, t - 0.02f, rt)) continue;   // a wall is in front of it
        bestT = t; best = (int)i;
    }
    if (best < 0) return -1;
    Prop& p = L.sc->props[best];
    if (p.type == 4 && !L.hasKey) return -1;
    if (p.type == 1) {
        for (size_t d = 0; d < L.doorProp.size(); d++)
            if (L.doorProp[d] == best && L.doors[d].locked) return -1;
    }
    return best;
}

// FirstPersonController.WinGame: wonGame, save progress, winPanel after 4 s
// (WinGameCor) or 20 s on level 6 (WinGameLongCor); the level keeps running
static void winLevel() {
    if (L.won) return;
    L.won = true;
    L.wonT = L.sc->level == 6 ? 20.f : 4.f;
    if (L.idx + 1 >= g_unlocked) { g_unlocked = std::min(7, L.idx + 2); g_dirtySave = true; }
}

static void die() {
    if (state == ST_DEAD || L.won) return;   // NeighborAI: !player.gameOver && !player.wonGame
    state = ST_DEAD;
    stateT = 0;
    stopMusic();
}

static void doInteract() {
    // FirstPersonController: holding an object only blocks taking another one
    // (findInteractable skips "Take" objects then); doors, locks and triggers still work
    Vec3 eye = eyePos();
    int idx = findInteractable();

    // key / scissors / money / car / wires triggers
    for (auto& m : L.sc->markers) {
        if (m.type == 5 && !L.carBurned && L.hasGas && inTrigger(m, 0.3f)) {
            L.carBurned = true; L.fireT = 10.f; return;
        }
        if (m.type == 6 && !L.wiresCut && L.hasScissors && inTrigger(m, 0.3f)) {
            L.wiresCut = true;
            if (L.sc->level == 2 || L.sc->level == 7) winLevel();
            return;
        }
    }

    if (idx < 0) return;
    Prop& p = L.sc->props[idx];
    if (p.type == 2) {
        L.props[idx].held = true;
        L.heldIdx = idx;
        L.props[idx].vel = Vec3();
        std::string nm = p.name;
        if (nm.find("Gas") != std::string::npos) L.hasGas = true;
        return;
    }
    if (p.type == 4) {
        if (!L.hasKey) return;
        L.lockOpened = true;
        float best = 4.f; int bestDoor = -1;
        for (size_t d = 0; d < L.doors.size(); d++) {
            float dd = dist2D(L.sc->props[L.doorProp[d]].pos, p.pos);
            if (dd < best) { best = dd; bestDoor = (int)d; }
        }
        if (bestDoor >= 0) L.doors[bestDoor].locked = false;
        L.props[idx].dropped = true;          // tag "Untagged", rigid.isKinematic = false
        L.props[idx].rest = false;
        L.props[idx].vel = Vec3(0, -0.01f, 0);
        if (L.sc->level == 1 || L.sc->level == 6) winLevel();
        return;
    }
    if (p.type == 1) {
        int di = -1;
        for (size_t d = 0; d < L.doorProp.size(); d++) if (L.doorProp[d] == idx) di = (int)d;
        if (di < 0) return;
        if (L.doors[di].locked) return;
        L.doors[di].openTarget = !L.doors[di].openTarget;
        audioPlay("СкрипДвери", 1.f);   // the player is next to it
    }
}

static void throwHeld() {
    if (L.heldIdx < 0) return;
    int i = L.heldIdx;
    Prop& p = L.sc->props[i];
    L.props[i].held = false;
    L.heldIdx = -1;
    Vec3 f = camForward();
    PropRT& r = L.props[i];
    // the object leaves from HoldPlacePoint with its held rotation (see renderWorld)
    float ph = -P.pitch * 0.5f;
    Quat camQ = qmul(Quat{0, sinf(P.yaw * 0.5f), 0, cosf(P.yaw * 0.5f)}, Quat{sinf(ph), 0, 0, cosf(ph)});
    Quat hold{-0.667f, -0.412f, -0.174f, 0.596f};
    r.rot = qnorm(qmul(qmul(camQ, hold), Quat{-p.restRot.x, -p.restRot.y, -p.restRot.z, p.restRot.w}));
    r.pos = eyePos() + qrot(camQ, Vec3(0.444f, -0.886f, 1.18f));
    r.vel = Vec3(f.x * 10.f, f.y + 5.f, f.z * 10.f);   // AddForce(impulse), mass 1
    r.angVel = Vec3();
    r.drag = 0.05f; r.angDrag = 0.f;
    r.rest = false; r.sleepT = 0.f;
    std::string nm = p.name;
    if (nm.find("Gas") != std::string::npos) L.hasGas = false;
}

// ------------------------------------------------------------------ update
// ------------------------------------------------------------------ neighbour
static float g_nWalk = 0.f;   // Animator float "WalkSpeed"

// NavMeshAgent: path over the level's baked NavMesh (nav.cpp), accelerate at 8 m/s^2
// toward the next corner with auto braking, stay on the mesh surface, turn to travel

static void navStep(float dt) {
    float spd = L.nStun > 0 ? 0.f : L.nSpeed;
    Vec3 want;
    if (navReady()) {
        g_nRepath -= dt;
        if (g_nPath.empty() || g_nRepath <= 0 || (L.nDest - g_nPathDest).len() > 0.4f) {
            navPath(L.nPos, L.nDest, g_nPath);
            g_nPathI = 1; g_nPathDest = L.nDest; g_nRepath = 0.5f;
        }
        while (g_nPathI + 1 < g_nPath.size() && dist2D(L.nPos, g_nPath[g_nPathI]) < 0.15f) g_nPathI++;
        if (g_nPathI < g_nPath.size()) {
            Vec3 c = g_nPath[g_nPathI];
            Vec3 to = c - L.nPos; to.y = 0;
            float d = sqrtf(to.x * to.x + to.z * to.z);
            float remain = d;
            for (size_t k = g_nPathI + 1; k < g_nPath.size(); k++) remain += dist2D(g_nPath[k - 1], g_nPath[k]);
            if (remain > std::max(0.05f, L.nStopDist) && spd > 0 && d > 1e-3f) {
                float brake = sqrtf(2.f * 8.f * std::max(0.f, remain - L.nStopDist));
                want = Vec3(to.x / d, 0, to.z / d) * std::min(spd, brake);
            }
        }
    }
    Vec3 dv = want - L.nVel;
    float dl = dv.len(), maxd = 8.f * dt;
    if (dl > maxd) dv = dv * (maxd / dl);
    L.nVel += dv;
    L.nVel.y = 0;
    L.nPos += L.nVel * dt;
    Vec3 sn;
    if (navReady() && navFindPoly(L.nPos, &sn) >= 0) L.nPos = sn;
    float sp = sqrtf(L.nVel.x * L.nVel.x + L.nVel.z * L.nVel.z);
    if (sp > 0.1f) {
        float target = atan2f(L.nVel.x, L.nVel.z);
        float diff = remainderf(target - L.nYaw, 6.2831853f);
        float maxr = 6.f * dt;
        L.nYaw += clampf(diff, -maxr, maxr);
    }
    // AutoOpenDoor: a door opens when he enters its trigger and closes when he leaves
    // (DoorControl.OpenDoor has no lock check)
    for (size_t d = 0; d < L.doors.size(); d++) {
        const Vec3& dp = L.props[L.doorProp[d]].pos;
        bool inside = dist2D(dp, L.nPos) < 1.6f && fabsf(dp.y - L.nPos.y) < 2.5f;
        DoorRT& dr = L.doors[d];
        if (inside && !dr.nbInside) { dr.openTarget = true; audioPlay("СкрипДвери", clampf(1.f - (dp - P.pos).len() / 7.f, 0, 1)); }
        if (!inside && dr.nbInside) { dr.openTarget = false; audioPlay("СкрипДвери", clampf(1.f - (dp - P.pos).len() / 7.f, 0, 1)); }
        dr.nbInside = inside;
    }
}

// NeighborAI.Update, line for line (the coroutines are timers)
static void updateNeighbor(float dt) {
    const CharLevel* cl = L.nb;
    float vmag = L.nVel.len();
    // footsteps: his AudioSource plays stepSound (Footstep02) while moving
    L.nStepT -= dt;
    if (vmag > 0.2f && L.nStepT <= 0) {
        float vol = clampf(1.f - (L.nPos - P.pos).len() / 17.f, 0.f, 1.f);
        if (vol > 0.02f) audioPlay("Footstep02", vol);
        L.nStepT = 0.55f;
    }
    if (L.nStun > 0) L.nStun -= dt;
    if (L.nCatch > 0) L.nCatch -= dt;
    if (L.wpStay > 0) L.wpStay -= dt;
    bool inStun = L.nStun > 0, isCatching = L.nCatch > 0, stay = L.wpStay > 0;
    int nwp = (int)cl->waypoints.size();

    if (!L.nAttack && nwp > 0 && !inStun) {
        const Vec3& wp = cl->waypoints[L.wpIndex];
        if (((wp - L.nPos).len() < 0.5f || (dist2D(wp, L.nPos) < 0.5f && fabsf(wp.y - L.nPos.y) < 1.2f)) && !stay) {
            L.wpIndex = (L.wpIndex + 1) % nwp;
            g_nWalk = 0.f;
            L.wpStay = 3.f; stay = true;
        }
        if (!stay) { L.nDest = cl->waypoints[L.wpIndex]; g_nWalk = vmag; }
    }

    // Physics.Raycast(position + up, target - position, seeDistance): first hit
    Vec3 tgt = P.pos + Vec3(0, 0.9f, 0);           // FPSController transform (capsule centre)
    Vec3 vec = tgt - L.nPos;
    Vec3 fwd(sinf(L.nYaw), 0, cosf(L.nYaw));
    float angle = acosf(clampf(dot(vec.norm(), fwd), -1.f, 1.f)) * 57.29578f;
    // Physics.Raycast(position + up, ...) towards the player: the first thing hit is
    // either level geometry or the player's CharacterController (h 1.8, r 0.41 + skin)
    Vec3 ro = L.nPos + Vec3(0, 1, 0), dir = (tgt - ro).norm();
    float tW = 0;
    bool hitW = g_tg.raycast(ro, dir, cl->seeDistance, tW);
    if (!hitW) tW = cl->seeDistance;
    // intact window panes (GlassCinema BoxCollider) and doors stop the ray too
    for (size_t k = 0; k < L.glassBroken.size(); k++) {
        Vec3 mn, mx; float t;
        if (!L.glassBroken[k] && glassBox((int)k, mn, mx) && rayAABB(ro, dir, mn, mx, t) && t < tW) { tW = t; hitW = true; }
    }
    for (size_t d = 0; d < L.doorProp.size(); d++) {
        float t;
        if (rayHitsProp((size_t)L.doorProp[d], ro, dir, t) && t > 0.f && t < tW) { tW = t; hitW = true; }
    }
    float tc = dot(tgt - ro, dir);
    bool hitP = tc > 0 && tc - 0.49f < cl->seeDistance && (!hitW || tW > tc - 0.49f);
    float pd = vec.len();

    if (hitW || hitP) {
        if (hitP && !isCatching && angle < 160.f && !inStun) {
            L.nCatch = 13.f; isCatching = true;         // StartCatch()
            L.nAttack = true; L.wpStay = 0;
            L.nDest = tgt; L.nSpeed = 5.f; g_nWalk = vmag;
            if (pd < cl->attackDistance && !inStun) { L.nStopDist = 3.f; g_nWalk = 0; if (state == ST_PLAY) die(); }
        } else if (!isCatching && !inStun) {
            L.nSpeed = 2.f; L.nAttack = false;
            setMusic("MENU");                           // soundControl.PlayMusicSound()
        }
        if (isCatching && !inStun) {
            if (!L.won) setMusic("Загрузка");           // soundControl.PlayCatchSound()
            L.nAttack = true; L.wpStay = 0;
            L.nDest = tgt; L.nSpeed = 4.f; g_nWalk = vmag;
            if (pd < cl->attackDistance) { L.nStopDist = 3.f; g_nWalk = 0; if (state == ST_PLAY) die(); }
        }
    }
    navStep(dt);
    charAnimUpdate(L.nAnim, dt, g_nWalk, L.nStun > 0);
}

static void updatePlay(float dt) {
    applyHudTaps();
    if (g_promptT > 0) g_promptT -= dt;
    // --- player look / move
    // stick: rad/s at full deflection with a curve for fine aiming near the centre;
    // touch/mouse: lookX/lookY are screen pixels, converted to radians per pixel
    const float STICK_YAW = 2.6f, STICK_PITCH = 1.8f, PIXEL_RAD = 0.0035f;
    auto curve = [](float v) { return v * fabsf(v); };
    P.yaw += curve(in.rx) * STICK_YAW * g_sens * dt + in.lookX * PIXEL_RAD;
    P.pitch -= curve(in.ry) * STICK_PITCH * g_sens * dt + in.lookY * PIXEL_RAD;
    P.pitch = clampf(P.pitch, -1.35f, 1.35f);

    Vec3 move(in.mx, 0, in.run ? -1.f : in.my);   // GetInput: running forces full forward
    float mlen = sqrtf(move.x * move.x + move.z * move.z);
    if (mlen > 1) { move.x /= mlen; move.z /= mlen; mlen = 1; }
    Vec3 fwd(sinf(P.yaw), 0, cosf(P.yaw));
    Vec3 right(cosf(P.yaw), 0, -sinf(P.yaw));
    float speed = in.run ? 5.0f : 3.0f;
    Vec3 wish = fwd * (-move.z) + right * move.x;
    P.vel.x = wish.x * speed * mlen;
    P.vel.z = wish.z * speed * mlen;
    // grounded: m_MoveDir.y = -m_StickToGroundForce (10); airborne: Physics.gravity * m_GravityMultiplier (1)
    if (P.grounded) P.vel.y = -10.f; else P.vel.y -= 9.81f * dt;
    // m_JumpSound = Footstep_concrete02, m_JumpSpeed 5
    if (P.grounded && in.bPress) { P.vel.y = 5.f; P.grounded = false; P.jumping = true; audioPlay("Footstep_concrete02", FOOT_VOL); }
    bool wasGrounded = P.grounded;
    Vec3 posBefore = P.pos;
    moveCapsule(P.pos, P.vel, 0.34f, 1.75f, dt, P.grounded);
    // CharacterController (radius 0.41, step offset 0.3): spheres up the body against
    // the level walls and the doors' current boxes
    for (int it = 0; it < 3; it++) {
        static const float hs[3] = {0.95f, 1.25f, 1.4f};   // clear of 0.3 m steps
        Vec3 total;
        for (float h : hs) {
            Vec3 c = P.pos + Vec3(0, h, 0) + total;
            total += g_tg.pushOut(c, 0.41f);
        }
        // closed doors and the rigidbody objects ("Take": drums, cans, tanks) block the
        // CharacterController like their colliders do in Unity
        static std::vector<int> blockers;
        blockers.clear();
        for (size_t d = 0; d < L.doorProp.size(); d++) blockers.push_back(L.doorProp[d]);
        for (size_t k = 0; k < L.sc->props.size(); k++)
            if (L.sc->props[k].type == 2 && L.props[k].alive && !L.props[k].held &&
                dist2D(L.props[k].pos, P.pos) < 4.f) blockers.push_back((int)k);
        for (size_t d = 0; d < blockers.size(); d++) {
            int pi = blockers[d];
            for (float h : hs) {
                Vec3 c = P.pos + Vec3(0, h, 0) + total, q;
                if (!closestOnProp((size_t)pi, c, q)) continue;
                // a real sphere contact (3D): doors and objects on another floor must not block
                if ((c - q).len() >= 0.41f) continue;
                Vec3 dv = c - q; dv.y = 0;
                float dl = dv.len();
                if (dl < 0.41f && dl > 1e-5f) total += dv * ((0.41f - dl) / dl);
            }
        }
        if (total.x == 0.f && total.z == 0.f) break;
        P.pos.x += total.x; P.pos.z += total.z;
    }
    // intact window panes (GlassCinema BoxCollider) push the player back out
    for (size_t k = 0; k < L.glassBroken.size(); k++) {
        Vec3 mn, mx;
        if (L.glassBroken[k] || !glassBox((int)k, mn, mx)) continue;
        const float r = 0.34f;
        if (P.pos.x + r <= mn.x || P.pos.x - r >= mx.x || P.pos.z + r <= mn.z || P.pos.z - r >= mx.z ||
            P.pos.y + 1.75f <= mn.y || P.pos.y >= mx.y) continue;
        float px1 = (P.pos.x + r) - mn.x, px2 = mx.x - (P.pos.x - r);
        float pz1 = (P.pos.z + r) - mn.z, pz2 = mx.z - (P.pos.z - r);
        float px = std::min(px1, px2), pz = std::min(pz1, pz2);
        if (px < pz) P.pos.x += px1 < px2 ? -px1 : px2;
        else P.pos.z += pz1 < pz2 ? -pz1 : pz2;
    }
    P.airT = P.grounded ? 0.f : P.airT + dt;
    // landing: JumpBob + PlayLandingSound (m_LandSound = Footstep_concrete02);
    // short airborne blips on stairs are ignored
    if (P.grounded && !wasGrounded && (P.jumping || P.lastAir > 0.25f)) {
        P.jumping = false; P.landT = 0.f;
        audioPlay("Footstep_concrete02", FOOT_VOL);
        P.nextStep = P.stepCycle + 0.5f;
    }
    P.lastAir = P.airT;
    // ProgressStepCycle: m_IsWalking is serialized false, so m_RunstepLenghten (0.7)
    // always scales the speed term; a step every m_StepInterval (5) units
    {
        float hv = dt > 0.f ? dist2D(P.pos, posBefore) / dt : 0.f;   // CharacterController.velocity
        float cyc = hv + speed * 0.7f;
        if (hv > 0.f && mlen > 0.f) P.stepCycle += cyc * dt;
        if (P.stepCycle > P.nextStep) {
            P.nextStep = P.stepCycle + 5.f;
            // Random.Range(1, 2) always picks slot 1, then swaps it to slot 0: the two
            // clips alternate, starting with Footstep_concrete02
            if (P.grounded) {
                audioPlay(P.stepAlt ? "Footstep_concrete01" : "Footstep_concrete02", FOOT_VOL);
                P.stepAlt = !P.stepAlt;
            }
        }
        // CurveControlledBob: Bobcurve (0,0)(.5,1)(1,0)(1.5,-1)(2,0), ranges 0.05,
        // vertical at twice the horizontal rate, base interval = m_StepInterval
        if (hv > 0.f && P.grounded) {
            P.bobCX += cyc * dt / 5.f;
            P.bobCY += cyc * dt / 5.f * 2.f;
            if (P.bobCX > 2.f) P.bobCX -= 2.f;
            if (P.bobCY > 2.f) P.bobCY -= 2.f;
            P.bobX = sinf(P.bobCX * 3.14159265f) * 0.05f;
            P.bobY = sinf(P.bobCY * 3.14159265f) * 0.05f;
        } else {
            P.bobX = P.bobY = 0.f;
        }
        if (P.landT >= 0.f) { P.landT += dt; if (P.landT > 0.4f) P.landT = -1.f; }
    }
    if (P.pos.y < -30.f) { P.pos = L.sc->spawn; P.vel = Vec3(); }

    // --- doors
    for (size_t d = 0; d < L.doors.size(); d++) {
        DoorRT& dr = L.doors[d];
        float target = dr.openTarget ? 1.f : 0.f;
        if (dr.open < target) dr.open = std::min(target, dr.open + dt / 0.55f);
        else if (dr.open > target) dr.open = std::max(target, dr.open - dt / 0.7333f);
    }

    // --- takeable prop physics
    for (size_t i = 0; i < L.props.size(); i++) {
        Prop& p = L.sc->props[i];
        PropRT& r = L.props[i];
        if (p.type != 2 && !r.dropped) continue;
        if (!r.alive || r.held) continue;
        if (r.rest) continue;
        stepBody(p, r, dt * 0.5f);   // two substeps per frame
        stepBody(p, r, dt * 0.5f);
        Vec3 comw = r.pos + qrot(r.rot, p.center);
        float rad = std::max(0.08f, std::min(fabsf(p.half.x), fabsf(p.half.z)));
        // a thrown "Take" object (useGravity, not held) entering a window's trigger
        if (p.type == 2 && r.vel.len() > 0.5f) {
            Vec3 c = comw;
            for (size_t k = 0; k < L.glassBroken.size(); k++) {
                Vec3 mn, mx;
                if (L.glassBroken[k] || !glassBox((int)k, mn, mx)) continue;
                if (c.x + rad > mn.x && c.x - rad < mx.x && c.y + rad > mn.y && c.y - rad < mx.y &&
                    c.z + rad > mn.z && c.z - rad < mx.z) breakGlass((int)k);
            }
        }
        // NeighborAI.OnTriggerEnter: a thrown "Take" object faster than 3 m/s stuns
        // him (Stunning coroutine: nav.speed = 0, Dizzy for 4 s)
        if (L.neighbor >= 0 && L.nStun <= 0 && r.vel.len() > 3.f && dist2D(comw, L.nPos) < 0.7f &&
            comw.y > L.nPos.y && comw.y < L.nPos.y + 2.2f) {
            L.nStun = 4.f;
            L.nSpeed = 0.f;
            r.vel = r.vel * 0.2f;
        }
    }

    if (L.neighbor >= 0) updateNeighbor(dt);

    // FirstPersonController.OnTriggerEnter: Key / Scissors / MoneyLoot are taken by
    // walking into them (the object is destroyed, its UI icon shown)
    for (auto& m : L.sc->markers) {
        if (m.type != 7 && m.type != 8 && m.type != 9) continue;
        if (dist2D(P.pos, m.pos) > 1.0f || fabsf(m.pos.y - (P.pos.y + 0.9f)) > 1.6f) continue;
        const char* nm = m.type == 7 ? "Key" : m.type == 8 ? "Scissors" : "MoneyLoot";
        bool& flag = m.type == 7 ? L.hasKey : m.type == 8 ? L.hasScissors : L.hasMoney;
        if (flag) continue;
        flag = true;
        for (size_t i = 0; i < L.sc->props.size(); i++)
            if (L.sc->props[i].type == 5 && strcmp(L.sc->props[i].name, nm) == 0) L.props[i].alive = false;
    }
    if (L.won && L.wonT > 0) {
        L.wonT -= dt;
        if (L.wonT <= 0) { state = ST_WON; stateT = 0; }
    }

    // --- car fire timer
    if (L.fireT > 0) {
        L.fireT -= dt;
        if (L.fireT <= 0) winLevel();
    }

    // --- home trigger (level 3/5: money; generic: all three)
    for (auto& m : L.sc->markers) {
        if (m.type != 4) continue;
        if (inTrigger(m, 0.f)) {
            int lv = L.sc->level;
            if ((lv == 3 || lv == 5) && L.hasMoney) winLevel();
            if (L.carBurned && L.wiresCut && L.hasMoney) winLevel();
        }
    }

    if (in.aPress) doInteract();
    if (in.xPress) throwHeld();
    if (in.yPress) { state = ST_TASKS; stateT = 0; }
    if (in.startPress) { state = ST_TASKS; stateT = 0; g_panelSel = 1; }   // pause
}

// ------------------------------------------------------------------ render
static void drawSky(const Vec3& fog) {
    // full screen clear already; draw a subtle gradient quad
    gfxBegin2D(W, H);
    gfxDrawRect(0, 0, (float)W, (float)H * 0.6f, Color(fog.x * 1.6f, fog.y * 1.6f, fog.z * 1.7f, 1.f), nullptr);
    gfxDrawRect(0, (float)H * 0.6f, (float)W, (float)H * 0.4f, Color(fog.x * 0.8f, fog.y * 0.8f, fog.z * 0.9f, 1.f), nullptr);
}

static void modelForProp(Prop& p, PropRT& r, Mat4& out, bool doorOpen, float doorT) {
    // DoorOpenAnim / DoorCloseAnim: localEulerAngles (-90, 0, -180) <-> (-90, -90, -180),
    // i.e. a -90 degree turn about the vertical axis in front of the rest rotation
    Mat4 rot = matFromQuat(r.rot);
    if (doorOpen && doorT != 0.f) rot = matRotY(-1.5707963f * doorT).mul(rot);
    out = matTranslate(r.pos).mul(rot).mul(matScale(p.scale));
}

// GameOverCamera shot (ST_DEAD) replaces the player camera
static bool g_camOverride = false;
static Vec3 g_camEye, g_camFwd;
static float g_camFov = 60.f;

static void renderWorld() {
    Scene* s = L.sc;
    Vec3 eye = eyePos();
    {
        // head bob (camera-local x/y) minus the landing JumpBob (0.2 s up, 0.2 s down, 0.05)
        float jb = P.landT < 0.f ? 0.f : (P.landT < 0.2f ? P.landT / 0.2f : (0.4f - P.landT) / 0.2f) * 0.05f;
        eye.x += cosf(P.yaw) * P.bobX; eye.z += -sinf(P.yaw) * P.bobX;
        eye.y += P.bobY - jb;
    }
    Vec3 dir = camForward();
    float fov = 60.f;   // FirstPersonCharacter camera: 60 deg vertical FOV, far plane 800
    if (g_camOverride) { eye = g_camEye; dir = g_camFwd; fov = g_camFov; }
    Mat4 view = matLookAt(eye, eye + dir, Vec3(0, 1, 0));
    Mat4 proj = matPerspective(fov, (float)W / H, 0.3f, 800.f);
    Mat4 vp = proj.mul(view);

    // the original scenes have fog disabled (RenderSettings.m_Fog = false)
    gfxBeginFrame(s->ambient, s->fog, 0.f);
    Mat4 viewRot = view;
    viewRot.m[12] = viewRot.m[13] = viewRot.m[14] = 0.f;
    gfxDrawSkybox(proj.mul(viewRot));
    gfxSetCamera(vp);
    gfxSetCameraPos(eye);

    // FirstPersonController.CutTheWires: houseLights.SetActive(false) switches this sun off
    g_switchLight = Vec3();
    if (!L.wiresCut)
        for (auto& lt : s->lights)
            if (lt.type == 100) g_switchLight = lt.color * lt.intensity;

    // opaque + cutout first, Unity "Transparent" materials after
    for (int pass = 0; pass < 2; pass++) {
        for (int mid : s->meshIds) {
            Mesh* m = getMesh(mid);
            if (!m) continue;
            Material* mat = getMaterial(m->matId);
            bool transp = mat && mat->cutout == 2;
            if (transp || pass == 1) continue;   // transparent level surfaces: drawn last
            float at = (mat && mat->cutout == 1) ? mat->cutoff : 0.f;
            gfxDrawMesh(m, Mat4(), Color(1, 1, 1, 1), at, transp);
        }
    }

    // props (type 13 parts move with their parent); opaque now, transparent at the end
    struct TDraw { Mesh* m; Mat4 model; float d; };
    std::vector<TDraw> transp;
    for (size_t i = 0; i < s->props.size(); i++) {
        size_t owner = i;
        Prop& p = s->props[i];
        if (p.type == 13) {
            if (p.parent < 0 || p.parent >= (int)s->props.size()) continue;
            owner = (size_t)p.parent;
        }
        Prop& op = s->props[owner];
        PropRT& r = L.props[owner];
        if (!r.alive || r.held) continue;
        if (op.type == 3 || op.type == 11 || op.type == 12) continue;   // neighbour parts (charDraw) / minimap / collision-only
        if (op.type == 6 && L.wiresCut) continue;   // houseLights group (level 7 dirt mounds)
        Mesh* m = getMesh(p.meshId);
        if (!m) continue;
        Material* mat = getMaterial(m->matId);
        Mat4 model;
        if (op.type == 1) {
            int di = -1;
            for (size_t d = 0; d < L.doorProp.size(); d++) if (L.doorProp[d] == (int)owner) di = (int)d;
            float t = di >= 0 ? L.doors[di].open : 0.f;
            modelForProp(op, r, model, true, t);
        } else {
            modelForProp(op, r, model, false, 0.f);
        }
        if (mat && mat->cutout == 2) {
            Vec3 c(model.m[12], model.m[13], model.m[14]);
            transp.push_back({m, model, (c - eye).len()});
            continue;
        }
        gfxDrawMesh(m, model, Color(1, 1, 1, 1), mat && mat->cutout == 1 ? mat->cutoff : 0.f, true);
    }

    // neighbour
    if (L.neighbor >= 0) {
        charDraw(L.nAnim, L.nPos, L.nYaw);   // skinned, animated (char.cpp)
    }
    // swaying trees (legacy Animation, skinned)
    if (L.sc) treesDraw(L.sc->level, g_time);

    // transparent: the level's surfaces, then props back to front
    for (int mid : s->meshIds) {
        Mesh* m = getMesh(mid);
        if (!m) continue;
        Material* mat = getMaterial(m->matId);
        if (mat && mat->cutout == 2) gfxDrawMesh(m, Mat4(), Color(1, 1, 1, 1), 0.f, true);
    }
    std::sort(transp.begin(), transp.end(), [](const TDraw& x, const TDraw& y) { return x.d > y.d; });
    for (auto& t : transp) gfxDrawMesh(t.m, t.model, Color(1, 1, 1, 1), 0.f, true);

    // held item: TakeObject parents it to the camera at HoldPlacePoint (local 0.444,
    // -0.886, 1.18) and Update turns it to that point's rotation
    if (L.heldIdx >= 0) {
        Prop& p = s->props[L.heldIdx];
        Mesh* m = getMesh(p.meshId);
        if (m) {
            float ph = -P.pitch * 0.5f;   // Unity camera pitch: +x looks down
            Mat4 cam = matTranslate(eye).mul(matRotY(P.yaw)).mul(matFromQuat(Quat{sinf(ph), 0, 0, cosf(ph)}));
            Quat hold{-0.667f, -0.412f, -0.174f, 0.596f};
            Quat inv{-p.restRot.x, -p.restRot.y, -p.restRot.z, p.restRot.w};   // undo the baked rest rotation
            Mat4 model = cam.mul(matTranslate(Vec3(0.444f, -0.886f, 1.18f))).mul(matFromQuat(hold)).mul(matFromQuat(inv));
            Material* mat = getMaterial(m->matId);
            gfxDrawMesh(m, model, Color(1, 1, 1, 1), mat && mat->cutout == 1 ? mat->cutoff : 0.f, true);
            for (size_t k = 0; k < s->props.size(); k++) {
                if (s->props[k].type != 13 || s->props[k].parent != L.heldIdx) continue;
                if (Mesh* pm = getMesh(s->props[k].meshId)) gfxDrawMesh(pm, model, Color(1, 1, 1, 1), 0.f, true);
            }
        }
    }

    gfxEnd3D();
}

static void drawIcon(const char* name, float x, float y, float size) {
    Texture* t = findTexture(name);
    if (t) {
        // keep aspect
        float ar = (float)t->h / (float)t->w;
        gfxDrawRect(x, y, size, size * ar, Color(1, 1, 1, 1), t);
    }
}

// ------------------------------------------------------------------ original HUD (Canvas)
// Everything below draws the level's own Canvas exported by work/make_hud.py: the
// minimap ring, TASKS button, run/jump buttons, joystick, crosshair, item icons,
// contextual take/door/lock/throw buttons and the parchment panels.
static float g_navCool = 0.f;

static const HudElement* HE(const char* path) { return L.sc ? hudFind(L.sc->level, path) : nullptr; }

static bool isUnder(const HudElement& e, const char* prefix) {
    size_t n = strlen(prefix);
    return e.path.compare(0, n, prefix) == 0 && (e.path.size() == n || e.path[n] == '/');
}

static bool tapped(const char* path) {
    const HudElement* e = HE(path);
    return in.tap && e && hudHit(*e, in.tapX, in.tapY);
}

// panel subtree (parchment, texts, buttons) in Canvas order; `selected` is highlighted
static void drawPanel(const char* prefix, const char* selected) {
    for (auto& e : hudElements()) {
        if (e.level != L.sc->level || !isUnder(e, prefix)) continue;
        bool sel = selected && (e.path == selected || isUnder(e, selected));
        if (e.kind == 1) hudDrawImage(e, sel ? Color(1.15f, 1.12f, 1.f, 1) : Color(1, 1, 1, 1), 1.f);
        else if (e.kind == 2) hudDrawText(e, nullptr, Color(1, 1, 1, 1));
    }
}

// two-button panel navigation: stick / d-pad moves the selection, returns the
// index confirmed with A or by touch (-1 if none this frame)
static int panelInput(const char* const* buttons, int n, bool vertical, float dt) {
    g_navCool -= dt;
    float axis = vertical ? in.my : in.mx;
    int dir = 0;
    if (in.dpadPress[vertical ? 0 : 2]) dir = -1;
    if (in.dpadPress[vertical ? 1 : 3]) dir = 1;
    if (!dir && fabsf(axis) > 0.6f && g_navCool <= 0) dir = axis > 0 ? 1 : -1;
    if (dir) { g_panelSel = (g_panelSel + dir + n) % n; g_navCool = 0.22f; }
    if (in.hover)
        for (int i = 0; i < n; i++) { const HudElement* e = HE(buttons[i]); if (e && hudHit(*e, in.hoverX, in.hoverY)) g_panelSel = i; }
    for (int i = 0; i < n; i++) if (tapped(buttons[i])) return i;
    if (in.aPress) return g_panelSel;
    return -1;
}

static void drawElem(const char* path, float alpha = 1.f) {
    const HudElement* e = HE(path);
    if (e) hudDrawImage(*e, Color(1, 1, 1, alpha));
}

static void drawMinimap(const HudElement& e);

static void renderHUD() {
    gfxBegin2D(W, H);
    int lv = L.sc->level;
    // minimap: masked disc, live top-down view, orange ring (Canvas/UIItems/Image*)
    for (auto& e : hudElements()) {
        if (e.level != lv) continue;
        if (e.path == "Canvas/UIItems/Image" && e.sprite != "whiteCircle") hudDrawImage(e, Color(1, 1, 1, 1));
        else if (e.path == "Canvas/UIItems/Image/RawImage") drawMinimap(e);
    }
    if (const HudElement* t = HE("Canvas/UIItems/TasksButton")) {
        hudDrawImage(*t, Color(1, 1, 1, 1));
        if (const HudElement* tt = HE("Canvas/UIItems/TasksButton/Text")) hudDrawText(*tt, nullptr, Color(1, 1, 1, 1));
    }
    // on-screen touch controls (joysticks, run, jump, action buttons) are not shown on Switch
    drawElem("Canvas/UIItems/Target");

    // inventory icons (top right), shown by the original scripts when picked up
    if (L.hasKey) drawElem("Canvas/UIItems/KeyImage");
    if (L.hasScissors || L.hasMoney) if (const HudElement* s = HE("Canvas/UIItems/ScissorsImage")) {
        HudElement c = *s; c.color.a = 1.f; hudDrawImage(c, Color(1, 1, 1, 1));
    }

    // short feedback line ("It's locked..."), fades out
    if (!L.prompt.empty() && g_promptT > 0) {
        const HudElement* tt = HE("Canvas/UIItems/TasksButton/Text");
        if (tt) {
            HudElement m = *tt;
            m.x = W * 0.2f; m.w = W * 0.6f; m.y = H * 0.70f; m.h = 48; m.minPx = 16; m.fontPx = 36;
            hudDrawText(m, L.prompt.c_str(), Color(1, 1, 1, clampf(g_promptT, 0, 1)));
        }
    }
}

// MiniMapCamera: orthographic (half-height 12 m), 100 m above the player, fixed
// orientation (screen-up = world -X, screen-right = +Z). Its culling mask skips the
// Default layer and it clears to the skybox, so it shows the sky's down face with
// the player and neighbour markers on top.
// floor the player / neighbour stands on (FirstFloorCol / SecondFloorCol / ThirdFloorCol
// triggers at y 0.41 / 5.19 / 9.6)
static int floorOf(float y) { return y < 2.8f ? 1 : y < 7.4f ? 2 : 3; }

static Texture* renderMinimapTex() {
    const int S = 256;
    const float half = 12.f;
    Texture* rt = gfxOffscreenBegin(S);
    Quat q{0.5f, -0.5f, 0.5f, 0.5f};
    Mat4 R = matFromQuat(q);
    Vec3 fwd(R.m[8], R.m[9], R.m[10]), up(R.m[4], R.m[5], R.m[6]);
    Vec3 eye = P.pos + Vec3(0, 1.087f + 100.f, 0);
    Mat4 view = matLookAt(eye, eye + fwd, up);
    Mat4 proj = matOrtho(-half, half, -half, half, 0.3f, 1000.f);
    Mat4 vp = proj.mul(view);
    gfxSetCamera(vp);
    gfxSetCameraPos(eye);
    g_switchLight = Vec3();
    Scene* s = L.sc;
    for (size_t i = 0; i < s->props.size(); i++) {
        Prop& p = s->props[i];
        PropRT& r = L.props[i];
        if (!r.alive || r.held) continue;
        if (p.type != 11 && p.type != 2) continue;   // layer 9 floor plans, layer 10 objects
        Mesh* m = getMesh(p.meshId);
        if (!m) continue;
        Mat4 model;
        modelForProp(p, r, model, false, 0.f);
        Material* mat = getMaterial(m->matId);
        gfxDrawMesh(m, model, Color(1, 1, 1, 1), mat && mat->cutout == 1 ? mat->cutoff : 0.f, true);
    }
    gfxEnd3D();
    // layer 8 sprites: MinimapSprite (green arrow, 4.1 m), MinimapArrowNeighbor (red, 2.73 m),
    // Up/DownArrow over the neighbour when he is on another floor
    gfxBegin2D(S, S);
    auto toPx = [&](const Vec3& w, float& x, float& y) {
        float cx = vp.m[0] * w.x + vp.m[4] * w.y + vp.m[8] * w.z + vp.m[12];
        float cy = vp.m[1] * w.x + vp.m[5] * w.y + vp.m[9] * w.z + vp.m[13];
        x = (cx * 0.5f + 0.5f) * S; y = (0.5f - cy * 0.5f) * S;
    };
    auto heading = [&](const Vec3& pos, float yaw) {
        float x0, y0, x1, y1;
        toPx(pos, x0, y0);
        toPx(pos + Vec3(sinf(yaw), 0, cosf(yaw)), x1, y1);
        return atan2f(x1 - x0, -(y1 - y0));
    };
    float ppm = S / (2.f * half);
    Texture* icon = findTexture("MiniMapIcon");
    Texture* arrow = findTexture("269061");
    if (L.neighbor >= 0) {
        float x, y;
        toPx(L.nPos, x, y);
        gfxDrawQuadRot(x, y, 2.73f * ppm, 2.73f * ppm, heading(L.nPos, L.nYaw), Color(1, 0, 0, 1), icon);
        int pf = floorOf(P.pos.y), nf = floorOf(L.nPos.y);
        if (pf != nf && arrow) {
            float aw = 5.12f * 0.6f * ppm;
            gfxDrawQuadRot(x, y, aw, aw * 204.f / 256.f, pf < nf ? -1.5707963f : 1.5707963f, Color(1, 1, 1, 1), arrow);
        }
    }
    {
        float x, y;
        toPx(P.pos, x, y);
        gfxDrawQuadRot(x, y, 4.096f * ppm, 4.096f * ppm, heading(P.pos, P.yaw), Color(0, 1, 0.007f, 1), icon);
    }
    gfxOffscreenEnd();
    return rt;
}

static void drawMinimap(const HudElement& e) {
    // the RawImage shows the MiniMapCamera RenderTexture through the round mask
    // the minimap RenderTexture is refreshed at 30 Hz (an offscreen pass per frame is
    // costly under emulation; the map moves slowly)
    static Texture* rt = nullptr;
    static int frame = 0;
    if (!rt || (++frame & 1) == 0) rt = renderMinimapTex();
    gfxBegin2D(W, H);
    gfxDrawRectMaskedUV(e.x, e.y, e.w, e.h, Color(1, 1, 1, 1), rt, findTexture("whiteCircle"), 0, 1, 1, 0);
    return;
}

static void drawMinimapOld(const HudElement& e) {
    Texture* mask = findTexture("whiteCircle");
    Texture* ground = findTexture("sky_down");
    gfxDrawRectMasked(e.x, e.y, e.w, e.h, Color(1, 1, 1, 1), ground, mask);
    float cx = e.x + e.w * 0.5f, cy = e.y + e.h * 0.5f;
    float ppm = (e.h * 0.5f) / 12.f;
    float dotR = e.w * 0.06f;
    if (L.neighbor >= 0) {
        float sx = (L.nPos.z - P.pos.z) * ppm, sy = (L.nPos.x - P.pos.x) * ppm;
        float d = sqrtf(sx * sx + sy * sy), lim = e.w * 0.5f - dotR;
        if (d > lim) { sx *= lim / d; sy *= lim / d; }   // pinned to the rim when out of range
        gfxDrawRect(cx + sx - dotR, cy + sy - dotR, dotR * 2, dotR * 2, Color(0.9f, 0.1f, 0.1f, 1), mask);
    }
    gfxDrawRect(cx - dotR, cy - dotR, dotR * 2, dotR * 2, Color(0.2f, 0.9f, 0.2f, 1), mask);
    // facing tick
    Vec3 f = camForward();
    float fx = f.z, fy = f.x, fl = sqrtf(fx * fx + fy * fy);
    if (fl > 1e-3f) {
        fx /= fl; fy /= fl;
        float r2 = dotR * 0.6f;
        gfxDrawRect(cx + fx * dotR * 1.9f - r2, cy + fy * dotR * 1.9f - r2, r2 * 2, r2 * 2, Color(0.2f, 0.9f, 0.2f, 1), mask);
    }
}

// touch on the original on-screen buttons maps onto the controller actions
static void applyHudTaps() {
    if (!in.tap) return;
    if (tapped("Canvas/UIItems/TasksButton")) in.yPress = true;
}
// ------------------------------------------------------------------ menu
// The Menu scene's own Canvas (hud.pak level 100) driven like Menu.cs: MainMenu with
// Play / Options / Quit, the options Panel (control type, volume slider, mute toggle)
// toggled by Menu.Option(), and LevelSelect with locked levels.
static int g_menuPage = 1;       // 1 = MainMenu, 2 = LevelSelect
static bool g_optOpen = false;   // Canvas/MainMenu/Panel
static float g_menuSaveT = -1;   // pending save after a settings change

enum { MI_PLAY, MI_OPTIONS, MI_QUIT, MI_SWIPE, MI_JOY, MI_VOLUME, MI_MUTE,
       MI_CONTROLS, MI_SCHEME, MI_SENS, MI_EDIT, MI_RESET, MI_LVL1 = 20, MI_BACK = 27 };
static bool g_ctlOpen = false;     // CONTROLS panel (right side, like the options Panel)
static int g_edRow = 0, g_edCol = 0;   // controls editor (menu page 3): row = action / back, col 0 keys 1 pad
static float g_edCapT = 0;
static const float QUIT_DY = 93.f;     // QUIT moves down one slot for CONTROLS

static const HudElement* ME(const char* path) { return hudFind(100, path); }
static bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

#ifdef __SWITCH__
static const bool kHasKeys = false;
static const char* const kSensLabel = "LOOK SENSITIVITY";
#else
static const bool kHasKeys = true;
static const char* const kSensLabel = "MOUSE SENSITIVITY";
#endif

static void menuItems(std::vector<int>& v) {
    v.clear();
    if (g_menuPage == 2) { for (int i = 0; i < 7; i++) v.push_back(MI_LVL1 + i); v.push_back(MI_BACK); return; }
    v.push_back(MI_PLAY); v.push_back(MI_OPTIONS); v.push_back(MI_CONTROLS); v.push_back(MI_QUIT);
    if (g_optOpen) { v.push_back(MI_SWIPE); v.push_back(MI_JOY); v.push_back(MI_VOLUME); v.push_back(MI_MUTE); }
    if (g_ctlOpen) { v.push_back(MI_SCHEME); v.push_back(MI_SENS); v.push_back(MI_EDIT); v.push_back(MI_RESET); }
}

// the selectable's GameObject, and the graphic its transition tints
static std::string menuPath(int it, bool graphic) {
    switch (it) {
        case MI_PLAY: return "Canvas/MainMenu/OpenLevelSelect";
        case MI_OPTIONS: return "Canvas/MainMenu/Options";
        case MI_QUIT: return "Canvas/MainMenu/Quit";
        case MI_SWIPE: return "Canvas/MainMenu/Panel/SwipeButton";
        case MI_JOY: return "Canvas/MainMenu/Panel/JoystickButton";
        case MI_VOLUME: return graphic ? "Canvas/MainMenu/Panel/Slider/Handle Slide Area/Handle" : "Canvas/MainMenu/Panel/Slider";
        case MI_MUTE: return graphic ? "Canvas/MainMenu/Panel/Mute Music/Background" : "Canvas/MainMenu/Panel/Mute Music";
        case MI_BACK: return "Canvas/LevelSelect/BackToMenu";
        case MI_CONTROLS: case MI_SCHEME: case MI_SENS: case MI_EDIT: case MI_RESET: return "";
    }
    char buf[48];
    snprintf(buf, sizeof buf, "Canvas/LevelSelect/%dLvl", it - MI_LVL1 + 1);
    return buf;
}

// ---- drawing helpers in the game's own style (button sprite, font 1, black text)
static void uiText(float x, float y, float w, float h, const std::string& s, float px, int align, const Color& c) {
    const HudElement* ref = ME("Canvas/MainMenu/Quit/Text");
    if (!ref) return;
    HudElement e = *ref;
    e.x = x; e.y = y; e.w = w; e.h = h; e.fontPx = px; e.minPx = px * 0.45f; e.align = align; e.color = c; e.text = s;
    hudDrawText(e, s.c_str(), Color(1, 1, 1, 1));
}
static void uiFrame(float x, float y, float w, float h, float t, const Color& c) {
    gfxDrawRect(x - t, y - t, w + 2 * t, t, c, nullptr);
    gfxDrawRect(x - t, y + h, w + 2 * t, t, c, nullptr);
    gfxDrawRect(x - t, y, t, h, c, nullptr);
    gfxDrawRect(x + w, y, t, h, c, nullptr);
}
static void uiButton(const RectF& r, const std::string& s, float px, bool sel, bool enabled = true, bool frame = false) {
    (void)frame;   // selection = brightness only (no outline)
    const HudElement* ref = ME("Canvas/MainMenu/Quit");
    if (!ref) return;
    HudElement b = *ref;
    b.x = r.x; b.y = r.y; b.w = r.w; b.h = r.h;
    Color tint = sel ? Color(1.15f, 1.15f, 1.15f, 1) : Color(1, 1, 1, 1);
    if (!enabled) tint = Color(0.78f, 0.78f, 0.78f, 0.6f);
    hudDrawImage(b, tint, 1.f);
    uiText(r.x + 10, r.y + 4, r.w - 20, r.h - 8, s, px, 4, Color(0, 0, 0, enabled ? 1.f : 0.6f));
}
static bool inRect(const RectF& r, float x, float y) { return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h; }

// CONTROLS panel layout (inside the options Panel's area, x 725..1280)
static const float CP_CX = 1002.8f;
static RectF ctlRect(int it) {
    switch (it) {
        case MI_SCHEME: return {CP_CX - 215, 200, 430, 74};
        case MI_SENS: return {CP_CX - 150, 352, 300, 44};
        case MI_EDIT: return {CP_CX - 160, 452, 320, 80};
        case MI_RESET: return {CP_CX - 160, 556, 320, 80};
    }
    return {};
}
static RectF mainBtnRect(int it) {
    const HudElement* q = ME("Canvas/MainMenu/Quit");
    RectF r; if (q) { r.x = q->x; r.y = q->y; r.w = q->w; r.h = q->h; }
    if (it == MI_QUIT) r.y += QUIT_DY;
    return r;
}

static bool menuHit(int it, float x, float y) {
    if (it == MI_CONTROLS || it == MI_QUIT) return inRect(mainBtnRect(it), x, y);
    if (it >= MI_SCHEME && it <= MI_RESET) return inRect(ctlRect(it), x, y);
    const HudElement* e = ME(menuPath(it, false).c_str());
    return e && hudHit(*e, x, y);
}

static void drawSlider(const RectF& r, float f, bool sel) {
    const HudElement* bg = ME("Canvas/MainMenu/Panel/Slider/Background");
    const HudElement* fill = ME("Canvas/MainMenu/Panel/Slider/Fill Area/Fill");
    const HudElement* ha = ME("Canvas/MainMenu/Panel/Slider/Handle Slide Area/Handle");
    if (!bg || !fill || !ha) return;
    HudElement b = *bg; b.x = r.x; b.y = r.y + (r.h - bg->h) * 0.5f; b.w = r.w;
    hudDrawImage(b, Color(1, 1, 1, 1));
    HudElement fl = *fill; fl.x = r.x; fl.y = b.y; fl.w = std::max(fill->h, f * r.w);
    hudDrawImage(fl, Color(1, 1, 1, 1));
    HudElement h = *ha; h.x = r.x + f * (r.w - ha->w); h.y = r.y + (r.h - ha->h) * 0.5f;
    hudDrawImage(h, sel ? Color(1.15f, 1.15f, 1.15f, 1) : Color(1, 1, 1, 1));
}

static void renderControlsPanel(int selIt) {
    if (const HudElement* p = ME("Canvas/MainMenu/Panel")) hudDrawImage(*p, Color(1, 1, 1, 1));
    const Color black(0, 0, 0, 1);
    uiText(CP_CX - 250, 80, 500, 60, "CONTROLS", 55.4f, 4, black);
    uiText(CP_CX - 250, 150, 500, 44, "SCHEME", 40.f, 4, black);
    std::string sch = g_scheme < SCHEME_N ? kSchemeName[g_scheme] : "CUSTOM";
    uiButton(ctlRect(MI_SCHEME), "<  " + sch + "  >", 38.f, selIt == MI_SCHEME, true, true);
    uiText(CP_CX - 250, 300, 500, 44, kSensLabel, 40.f, 4, black);
    RectF s = ctlRect(MI_SENS);
    drawSlider(s, (g_sens - SENS_MIN) / (SENS_MAX - SENS_MIN), selIt == MI_SENS);
    char v[16]; snprintf(v, sizeof v, "%.1f", g_sens);
    uiText(s.x + s.w + 12, s.y, 90, s.h, v, 38.f, 3, black);
    uiButton(ctlRect(MI_EDIT), "EDIT CONTROLS", 40.f, selIt == MI_EDIT, true, true);
    uiButton(ctlRect(MI_RESET), "RESET", 40.f, selIt == MI_RESET, true, true);
}

// ---- controls editor (page 3)
static const float ED_Y0 = 148, ED_DY = 47, ED_H = 40;
static RectF edCell(int row, int col) {
    if (row >= A_N) return {W * 0.5f - 125, 618, 250, 80};             // BACK
    float y = ED_Y0 + row * ED_DY;
    if (!kHasKeys) return {700, y, 300, ED_H};
    return col == 0 ? RectF{560, y, 280, ED_H} : RectF{870, y, 280, ED_H};
}
static bool edEditable(int row, int col) {
    if (row >= A_N) return true;
    if (row == A_PAUSE) return false;                                   // fixed: Esc / Start
    if (col == 1 && row <= A_RIGHT) return false;                       // the left stick moves
    return true;
}
static std::string edLabel(int row, int col) {
    if (col == 0) return keyName(g_bind.key[row]);
    if (row <= A_RIGHT) return "LEFT STICK";
    return padName(g_bind.pad[row]);
}

static void renderEditor() {
    if (const HudElement* bg = ME("Canvas/MainMenu/BackGround")) hudDrawImage(*bg, Color(1, 1, 1, 1));
    if (const HudElement* p = ME("Canvas/MainMenu/Panel")) {
        HudElement e = *p; e.x = 90; e.y = 8; e.w = 1100; e.h = 704;
        hudDrawImage(e, Color(1, 1, 1, 1));
    }
    const Color black(0, 0, 0, 1);
    uiText(140, 34, 1000, 56, "EDIT CONTROLS", 55.4f, 4, black);
    float nameX = kHasKeys ? 160 : 300;
    if (kHasKeys) uiText(560, 96, 280, 44, "KEYBOARD / MOUSE", 34.f, 4, black);
    {
        RectF c = edCell(0, 1);
        uiText(c.x, 96, c.w, 44, "CONTROLLER", 34.f, 4, black);
    }
    for (int r = 0; r < A_N; r++) {
        float y = ED_Y0 + r * ED_DY;
        uiText(nameX, y, 380, ED_H, kActName[r], 34.f, 3, black);
        for (int c = kHasKeys ? 0 : 1; c < 2; c++) {
            bool sel = g_edRow == r && g_edCol == c;
            std::string lab = (sel && g_edCapture) ? (c == 0 ? "PRESS A KEY..." : "PRESS A BUTTON...") : edLabel(r, c);
            uiButton(edCell(r, c), lab, 30.f, sel, edEditable(r, c), true);
        }
    }
    uiButton(edCell(A_N, 0), "BACK", 46.f, g_edRow == A_N, true, true);
    const char* hint = g_edCapture ? (g_edCol == 0 ? "ESC: CANCEL" : "START / +: CANCEL")
                                   : (kHasKeys ? "ENTER / CLICK: CHANGE     ESC: BACK" : "A: CHANGE     B: BACK");
    uiText(140, 690, 1000, 26, hint, 24.f, 4, Color(0, 0, 0, 0.8f));
}

static void edAssign(int row, int col, int code) {
    int* arr = col == 0 ? g_bind.key : g_bind.pad;
    // the same input on another action: swap them
    for (int a = 0; a < A_N; a++)
        if (a != row && arr[a] == code && edEditable(a, col)) arr[a] = arr[row];
    arr[row] = code;
    g_scheme = SCHEME_N;   // custom
    saveControls();
}

static void updateEditor(float dt) {
    if (!kHasKeys) g_edCol = 1;
    if (g_edCapture) {
        g_edCapT += dt;
        if (g_edCapT < 0.15f) return;   // ignore the press that started the capture
        if (g_edCol == 0 && g_capKey >= 0) {
            if (g_capKey != SDL_SCANCODE_ESCAPE) edAssign(g_edRow, 0, g_capKey);
            g_edCapture = false;
        } else if (g_edCol == 1 && g_capPad >= 0) {
            if (g_capPad != SDL_CONTROLLER_BUTTON_START) edAssign(g_edRow, 1, g_capPad);
            g_edCapture = false;
        } else if (g_edCapT > 8.f) g_edCapture = false;
        if (!g_edCapture) { prevA = prevB = true; }   // no confirm on release
        return;
    }
    static float cd = 0; cd -= dt;
    int dv = 0, dh = 0;
    if (in.dpadPress[0]) dv = -1;
    if (in.dpadPress[1]) dv = 1;
    if (in.dpadPress[2]) dh = -1;
    if (in.dpadPress[3]) dh = 1;
    if (!dv && fabsf(in.my) > 0.6f && cd <= 0) { dv = in.my > 0 ? 1 : -1; }
    if (!dh && fabsf(in.mx) > 0.6f && cd <= 0) { dh = in.mx > 0 ? 1 : -1; }
    if (dv || dh) cd = 0.22f;
    if (dv) g_edRow = (g_edRow + dv + A_N + 1) % (A_N + 1);
    if (dh && kHasKeys) g_edCol = (g_edCol + dh + 2) % 2;
    // mouse: hover selects a cell, a click changes it
    auto pick = [&](float x, float y, int& row, int& col) {
        for (int r = 0; r <= A_N; r++)
            for (int c = (r < A_N && kHasKeys) ? 0 : 1; c < 2; c++)
                if (inRect(edCell(r, c), x, y)) { row = r; col = c; return true; }
        return false;
    };
    int r, c;
    if (in.hover && pick(in.hoverX, in.hoverY, r, c)) { g_edRow = r; if (r < A_N) g_edCol = c; }
    bool act = in.aPress;
    if (in.tap) { act = pick(in.tapX, in.tapY, r, c); if (act) { g_edRow = r; if (r < A_N) g_edCol = c; } }
    if (in.bPress) { g_menuPage = 1; g_menuSel = 2; return; }
    if (act) {
        if (g_edRow >= A_N) { g_menuPage = 1; g_menuSel = 2; return; }
        if (edEditable(g_edRow, g_edCol)) { g_edCapture = true; g_edCapT = 0; }
    }
}

static void renderMenu() {
    gfxBegin2D(W, H);
    if (g_menuPage == 3) { renderEditor(); return; }
    std::vector<int> items; menuItems(items);
    if (g_menuSel >= (int)items.size()) g_menuSel = 0;
    int selIt = items[g_menuSel];
    std::string sel = menuPath(selIt, true);
    const char* root = g_menuPage == 2 ? "Canvas/LevelSelect" : "Canvas/MainMenu";
    const HudElement* fa = ME("Canvas/MainMenu/Panel/Slider/Fill Area");
    const HudElement* ha = ME("Canvas/MainMenu/Panel/Slider/Handle Slide Area");
    const float cs = 1.386f;   // CanvasScaler factor at 1280x720 (offsets below are canvas units)
    const Color disabled(0.78f, 0.78f, 0.78f, 0.5f);   // Selectable ColorBlock.disabledColor
    for (auto& e : hudElements()) {
        if (e.level != 100 || !startsWith(e.path, root)) continue;
        if (!g_optOpen && startsWith(e.path, "Canvas/MainMenu/Panel")) continue;
        bool isSel = !sel.empty() && e.path == sel;
        HudElement d = e;
        if (startsWith(e.path, "Canvas/MainMenu/Quit")) d.y += QUIT_DY;   // CONTROLS takes its slot
        if (e.kind == 2) { hudDrawText(d, nullptr, Color(1, 1, 1, 1)); continue; }
        if (e.kind != 1) continue;
        Color tint = isSel ? Color(1.15f, 1.15f, 1.15f, 1) : Color(1, 1, 1, 1);
        if (e.path == "Canvas/MainMenu/Panel/Mute Music/Background/Checkmark" && !g_mute) continue;
        // Slider: Fill spans the Fill Area up to the value (sizeDelta 10, left -5),
        // the Handle sits at the value inside the Handle Slide Area
        if (fa && e.path == "Canvas/MainMenu/Panel/Slider/Fill Area/Fill") { d.x = fa->x - 5 * cs; d.w = g_volume * fa->w + 10 * cs; }
        if (ha && e.path == "Canvas/MainMenu/Panel/Slider/Handle Slide Area/Handle") d.x = ha->x + g_volume * ha->w - d.w * 0.5f;
        // Menu.CheckControl: the active control type's button is non-interactable
        if ((e.path == "Canvas/MainMenu/Panel/SwipeButton" && g_ctrlType == 0) ||
            (e.path == "Canvas/MainMenu/Panel/JoystickButton" && g_ctrlType == 1))
            tint = Color(disabled.r * (isSel ? 1.15f : 1.f), disabled.g * (isSel ? 1.15f : 1.f), disabled.b * (isSel ? 1.15f : 1.f), disabled.a);
        // locked levels: SpriteSwap transition to the disabled sprite "closedlvl"
        int lv = 0;
        if (sscanf(e.path.c_str(), "Canvas/LevelSelect/%dLvl", &lv) == 1 && lv > g_unlocked) d.sprite = "closedlvl";
        hudDrawImage(d, tint, 1.f);   // selection = brightness only (no size change)
    }
    if (g_menuPage == 1) {
        // CONTROLS: the QUIT button's look in QUIT's old slot
        const HudElement* q = ME("Canvas/MainMenu/Quit");
        const HudElement* qt = ME("Canvas/MainMenu/Quit/Text");
        if (q && qt) {
            hudDrawImage(*q, selIt == MI_CONTROLS ? Color(1.15f, 1.15f, 1.15f, 1) : Color(1, 1, 1, 1), 1.f);
            hudDrawText(*qt, "CONTROLS", Color(1, 1, 1, 1));
        }
        if (g_ctlOpen) renderControlsPanel(selIt);
    }
}

static void menuStartLevel(int lv) {
    if (lv < 1 || lv > 7 || lv > g_unlocked) return;   // lvlN.interactable
    saveProgress();
    g_pendingLevel = lv - 1;
    state = ST_LOADING;
    g_loadT = 0;
    stopMusic();
}

static void stepScheme(int d) {
    int s = g_scheme >= SCHEME_N ? (d > 0 ? 0 : SCHEME_N - 1) : (g_scheme + d + SCHEME_N) % SCHEME_N;
    applyScheme(s);
    saveControls();
}

static void menuActivate(int it) {
    switch (it) {
        case MI_PLAY: g_menuPage = 2; g_menuSel = 0; return;                  // OpenLevelSelect
        case MI_OPTIONS: g_optOpen = !g_optOpen; g_ctlOpen = false; g_menuSaveT = 0; return;   // Option()
        case MI_CONTROLS: g_ctlOpen = !g_ctlOpen; g_optOpen = false; return;
        case MI_QUIT: saveProgress(); saveControls(); quitGame();
        case MI_SWIPE: g_ctrlType = 0; g_menuSaveT = 0; return;                // SetSwipeControl
        case MI_JOY: g_ctrlType = 1; g_menuSaveT = 0; return;                  // SetJoystickControl
        case MI_MUTE: g_mute = !g_mute; g_menuSaveT = 0; return;
        case MI_VOLUME: return;
        case MI_SCHEME: stepScheme(1); return;
        case MI_SENS: return;
        case MI_EDIT: g_menuPage = 3; g_edRow = 0; g_edCol = kHasKeys ? 0 : 1; g_edCapture = false; return;
        case MI_RESET: applyScheme(0); g_sens = 1.f; saveControls(); return;
        case MI_BACK: g_menuPage = 1; g_menuSel = 0; return;                   // GoToMainMenu
    }
    menuStartLevel(it - MI_LVL1 + 1);
}

static void updateMenu(float dt) {
    if (g_menuPage == 3) { updateEditor(dt); audioSetVolume(g_mute ? 0.f : g_volume); return; }
    std::vector<int> items; menuItems(items);
    int n = (int)items.size();
    if (g_menuSel >= n) g_menuSel = 0;
    static float cd = 0;
    cd -= dt;
    float v = in.my, h = in.mx;
    if (in.dpadPress[0]) v = -1;
    if (in.dpadPress[1]) v = 1;
    if (in.dpadPress[2]) h = -1;
    if (in.dpadPress[3]) h = 1;
    int it = items[g_menuSel];
    // level select: either axis steps through the scattered level buttons
    if (g_menuPage == 2 && fabsf(h) > 0.6f && fabsf(v) <= 0.6f) v = h, h = 0;
    // step on push, auto-repeat after 0.4 s while held
    static bool held = false;
    bool push = fabsf(v) > 0.6f;
    if (push && (!held || cd <= 0)) { g_menuSel = (g_menuSel + (v > 0 ? 1 : n - 1)) % n; cd = held ? 0.15f : 0.4f; }
    held = push;
    static bool hheld = false;
    bool hp = fabsf(h) > 0.6f;
    bool hEdge = hp && !hheld;
    hheld = hp;
    if (push) {
        // vertical navigation owns this frame
    } else if (it == MI_VOLUME && fabsf(h) > 0.2f) { g_volume = clampf(g_volume + h * dt * 0.7f, 0, 1); g_menuSaveT = 0; }
    else if (it == MI_SENS && fabsf(h) > 0.2f) { g_sens = clampf(g_sens + h * dt * 1.2f, SENS_MIN, SENS_MAX); g_menuSaveT = 0; }
    else if (it == MI_SCHEME && hEdge) stepScheme(h > 0 ? 1 : -1);
    else if ((it == MI_SWIPE || it == MI_JOY) && hEdge) {
        for (int i = 0; i < n; i++) if (items[i] == (it == MI_SWIPE ? MI_JOY : MI_SWIPE)) g_menuSel = i;
    }
    // mouse: hovering a button selects it
    if (in.hover)
        for (int i = 0; i < n; i++) if (menuHit(items[i], in.hoverX, in.hoverY)) { g_menuSel = i; break; }
    if (in.bPress) {
        if (g_menuPage == 2) { g_menuPage = 1; g_menuSel = 0; }
        else if (g_optOpen) { g_optOpen = false; g_menuSel = 1; }
        else if (g_ctlOpen) { g_ctlOpen = false; g_menuSel = 2; }
    } else if (in.aPress) {
        menuActivate(it);
    }
    // touch / mouse: tap the buttons; press / drag on the sliders sets the value
    if (in.tap) {
        for (int i = 0; i < n; i++) {
            if (!menuHit(items[i], in.tapX, in.tapY)) continue;
            g_menuSel = i;
            if (items[i] == MI_SCHEME) stepScheme(in.tapX < ctlRect(MI_SCHEME).x + ctlRect(MI_SCHEME).w * 0.3f ? -1 : 1);
            else if (items[i] != MI_VOLUME && items[i] != MI_SENS) menuActivate(items[i]);
            break;
        }
    }
    if (g_touchHeld && g_optOpen && g_menuPage == 1) {
        const HudElement* sl = ME("Canvas/MainMenu/Panel/Slider");
        const HudElement* ha = ME("Canvas/MainMenu/Panel/Slider/Handle Slide Area");
        if (sl && ha && g_touchY >= sl->y - 20 && g_touchY <= sl->y + sl->h + 20 &&
            g_touchX >= sl->x - 20 && g_touchX <= sl->x + sl->w + 20) {
            g_volume = clampf((g_touchX - ha->x) / std::max(1.f, ha->w), 0, 1);
            g_menuSaveT = 0;
        }
    }
    if (g_touchHeld && g_ctlOpen && g_menuPage == 1) {
        RectF s = ctlRect(MI_SENS);
        if (g_touchY >= s.y - 20 && g_touchY <= s.y + s.h + 20 && g_touchX >= s.x - 20 && g_touchX <= s.x + s.w + 20) {
            float f = clampf((g_touchX - s.x) / s.w, 0, 1);
            g_sens = SENS_MIN + f * (SENS_MAX - SENS_MIN);
            g_menuSaveT = 0;
        }
    }
    // Menu.Update: a ticked "Mute music" holds the volume slider at 0
    if (g_mute) g_volume = 0.f;
    audioSetVolume(g_volume);   // AudioListener.volume
    if (g_menuSaveT >= 0) { g_menuSaveT += dt; if (g_menuSaveT > 0.6f) { saveProgress(); saveControls(); g_menuSaveT = -1; } }
}

// LoadGame scene (hud.pak level 101): LOADING artwork and the orange filled bar
static void renderLoading() {
    gfxBegin2D(W, H);
    float f = clampf(g_loadT / 1.4f, 0, 1);
    const HudElement* bg = hudFind(101, "Canvas/Background");
    const HudElement* bar = hudFind(101, "Canvas/Image");
    const HudElement* fill = hudFind(101, "Canvas/Image/Image");
    if (!bg || !bar || !fill) {
        gfxDrawRect(0, 0, (float)W, (float)H, Color(0.02f, 0.02f, 0.03f, 1), nullptr);
        gfxDrawRect(W * 0.5f - 240, H * 0.55f, 480 * f, 18, Color(0.7f, 0.65f, 0.3f, 1), nullptr);
        return;
    }
    hudDrawImage(*bg, Color(1, 1, 1, 1));
    hudDrawImage(*bar, Color(1, 1, 1, 1));
    // Image.type Filled, horizontal from the left: fillAmount crops the sprite
    Texture* t = findTexture(fill->sprite.c_str());
    gfxDrawRect(fill->x, fill->y, fill->w * f, fill->h, fill->color, t, 0, 0, f, 1);
}

static void openMenu() {
    state = ST_MENU;
    g_menuPage = 1;
    g_optOpen = false;
    g_ctlOpen = false;
    g_menuSel = 0;
    stopMusic();
    audioPlay("music_cartoony_background", g_volume, true);
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    (void)argc; (void)argv;
#ifdef __SWITCH__
    romfsInit();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);   // thin walls 50+ m away z-fight on 16 bits
    SDL_Window* win = SDL_CreateWindow("Neighbor's Dark Riddle", 0, 0, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
#else
#ifdef _WIN32
    // the game's files (romfs/, saves, controls) sit next to the exe
    if (char* bp = SDL_GetBasePath()) { SetCurrentDirectoryA(bp); SDL_free(bp); }
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0");   // the driver's own OpenGL ES 2 (WGL)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");         // window / taskbar icon:
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");   // the exe's icon resource 1
#endif
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);   // thin walls 50+ m away z-fight on 16 bits
    const Uint32 winFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    SDL_Window* win = SDL_CreateWindow("Neighbor's Dark Riddle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, winFlags);
#endif
    if (!win) { printf("no window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
#ifdef _WIN32
    if (!ctx) {
        // no OpenGL ES 2 context from this driver: desktop OpenGL 2.1 runs the same shaders
        printf("[gl] no ES2 context (%s), trying desktop GL 2.1\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        win = SDL_CreateWindow("Neighbor's Dark Riddle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, winFlags);
        ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    }
#endif
    if (!ctx) {
        printf("no GL: %s\n", SDL_GetError());
#ifdef _WIN32
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Neighbor's Dark Riddle", "Could not create an OpenGL context.\nPlease update your graphics driver.", nullptr);
#endif
        return 1;
    }
    if (!drGLLoad()) {
        printf("[gl] missing OpenGL functions\n");
#ifdef _WIN32
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Neighbor's Dark Riddle", "Your OpenGL driver is missing required functions.\nPlease update your graphics driver.", nullptr);
#endif
        return 1;
    }
    printf("[gl] %s / %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
#ifdef __SWITCH__
    SDL_GL_SetSwapInterval(1);
#else
    {
        // vsync only on a 60 Hz display; on faster monitors the 60 FPS frame cap paces
        // the game (vsync at 120/144 Hz would make the capped frames uneven)
        SDL_DisplayMode dm;
        int hz = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 ? dm.refresh_rate : 60;
        SDL_GL_SetSwapInterval(hz >= 58 && hz <= 61 ? 1 : 0);
        printf("[gfx] display %d Hz, vsync %s\n", hz, (hz >= 58 && hz <= 61) ? "on" : "off");
    }
#endif

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
    }

    const char* pak =
#ifdef __SWITCH__
        "romfs:/data.pak";
#else
        "romfs/data.pak";
#endif
    const char* uiPath =
#ifdef __SWITCH__
        "romfs:/ui.pak";
#else
        "romfs/ui.pak";
#endif
    printf("[boot] loading %s\n", pak);
    if (!engineLoadPak(pak)) { printf("[boot] pak missing\n"); return 1; }
    uiLoadPak(uiPath);
    hudLoad(
#ifdef __SWITCH__
        "romfs:/hud.pak"
#else
        "romfs/hud.pak"
#endif
    );
    charLoad(
#ifdef __SWITCH__
        "romfs:/char.pak"
#else
        "romfs/char.pak"
#endif
    );
    treesLoad(
#ifdef __SWITCH__
        "romfs:/trees.pak"
#else
        "romfs/trees.pak"
#endif
    );
    navLoad(
#ifdef __SWITCH__
        "romfs:/nav.pak"
#else
        "romfs/nav.pak"
#endif
    );
    gfxInit(W, H);
    audioInit();
    loadProgress();
    loadControls();
    audioSetVolume(g_volume);

    state = ST_BOOT;
    stateT = 0;


    // frame time from the high-resolution counter; on PC the loop is held at 60 FPS
    // (the Switch's rate, which the game is tuned for) on any monitor / vsync setting
    const double freq = (double)SDL_GetPerformanceFrequency();
    Uint64 last = SDL_GetPerformanceCounter();
#ifndef __SWITCH__
    Uint64 nextFrame = last;
    const Uint64 frameTicks = (Uint64)(freq / 60.0);
#endif
    while (true) {
#ifndef __SWITCH__
        {
            nextFrame += frameTicks;
            Uint64 nowT = SDL_GetPerformanceCounter();
            if (nowT > nextFrame + frameTicks * 4) nextFrame = nowT;   // fell far behind: resync
            for (;;) {
                nowT = SDL_GetPerformanceCounter();
                if (nowT >= nextFrame) break;
                double leftMs = (double)(nextFrame - nowT) * 1000.0 / freq;
                if (leftMs > 2.0) SDL_Delay((Uint32)(leftMs - 1.5));
            }
        }
#endif
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - last) / freq);
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt <= 0) dt = 1.f / 60.f;
        g_time += dt;
        stateT += dt;

#ifndef __SWITCH__
        {
            // 16:9 game area letterboxed in the (resizable) window
            int dw = 1280, dh = 720, ww = 1280, wh = 720;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            SDL_GetWindowSize(win, &ww, &wh);
            float sc = std::min(dw / (float)W, dh / (float)H);
            int vw = std::max(1, (int)(W * sc + 0.5f)), vh = std::max(1, (int)(H * sc + 0.5f));
            int vx = (dw - vw) / 2, vy = (dh - vh) / 2;
            gfxSetScreenViewport(vx, vy, vw, vh, dw, dh);
            float k = ww / (float)std::max(1, dw);   // drawable pixels -> window points
            g_vpX = (int)(vx * k); g_vpY = (int)(vy * k); g_vpW = (int)(vw * k); g_vpH = (int)(vh * k);
            // the mouse is the camera while playing, a pointer everywhere else
            bool wantCap = state == ST_PLAY && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
            if (wantCap != g_mouseCaptured) {
                SDL_SetRelativeMouseMode(wantCap ? SDL_TRUE : SDL_FALSE);
                g_mouseCaptured = wantCap;
                if (wantCap) g_touchHeld = false;
            }
        }
#endif
        pollInput();

        switch (state) {
        case ST_BOOT:
            gfxBegin2D(W, H);
            gfxDrawRect(0, 0, (float)W, (float)H, Color(0.02f, 0.02f, 0.03f, 1), nullptr);
            if (stateT > 1.2f) openMenu();
            break;
        case ST_MENU:
            updateMenu(dt);
            renderMenu();
            break;
        case ST_LOADING:
            g_loadT += dt;
            renderLoading();
            if (g_loadT > 1.4f) startLevel(g_pendingLevel);
            break;
        case ST_PLAY:
            updatePlay(dt);
            renderWorld();
            renderHUD();
            if (g_dirtySave) { saveProgress(); g_dirtySave = false; }
            break;
        case ST_INTRO:
            // StartTasksPanel: the level's story parchment; any confirm / tap closes it
            renderWorld();
            gfxBegin2D(W, H);
            drawPanel("Canvas/UIItems/StartTasksPanel", nullptr);
            if (stateT > 0.5f && (in.aPress || in.bPress || in.tap || in.startPress)) { state = ST_PLAY; stateT = 0; }
            break;
        case ST_TASKS: {
            // TasksPanel: [Main Menu] [Close]
            renderWorld();
            renderHUD();
            static const char* const btn[2] = {"Canvas/UIItems/TasksPanel/GoToMenuButton",
                                               "Canvas/UIItems/TasksPanel/CloseTasksButton"};
            drawPanel("Canvas/UIItems/TasksPanel", btn[g_panelSel]);
            int pick = stateT > 0.2f ? panelInput(btn, 2, false, dt) : -1;
            if (in.yPress || in.bPress || (stateT > 0.2f && in.startPress)) pick = 1;
            if (pick == 0) openMenu();
            else if (pick == 1) { state = ST_PLAY; stateT = 0; }
            break;
        }
        case ST_WON: {
            // CompletePanel: THE MISSION IS COMPLETE!  [Continue] [Main Menu]
            renderWorld();
            gfxBegin2D(W, H);
            static const char* const btn[2] = {"Canvas/CompletePanel/RestartButton",
                                               "Canvas/CompletePanel/GoToMenuButton"};
            drawPanel("Canvas/CompletePanel", btn[g_panelSel]);
            int pick = stateT > 0.6f ? panelInput(btn, 2, true, dt) : -1;
            if (stateT > 0.6f && in.bPress) pick = 1;
            if (pick == 0) {
                if (L.idx + 1 < (int)g_scenes.size()) { g_pendingLevel = L.idx + 1; state = ST_LOADING; g_loadT = 0; }
                else openMenu();
            } else if (pick == 1) openMenu();
            break;
        }
        case ST_DEAD: {
            // GameOverCor: 2 s through the neighbour's GameOverCamera with the red
            // RedGameOver panel, then GameOverPanel: GAME OVER  [Restart] [Main Menu]
            if (stateT < 2.0f && L.neighbor >= 0) {
                charAnimUpdate(L.nAnim, dt, 0.f, false);
                charGameOverCamera(L.nPos, L.nYaw, g_camEye, g_camFwd, g_camFov);
                g_camOverride = true;
                renderWorld();
                g_camOverride = false;
                break;   // no red overlay
            }
            renderWorld();
            gfxBegin2D(W, H);
            static const char* const btn[2] = {"Canvas/GameOverPanel/RestartButton",
                                               "Canvas/GameOverPanel/GoToMenuButton"};
            drawPanel("Canvas/GameOverPanel", btn[g_panelSel]);
            int pick = stateT > 2.4f ? panelInput(btn, 2, true, dt) : -1;
            if (stateT > 2.4f && in.bPress) pick = 1;
            if (pick == 0) { g_pendingLevel = L.idx; state = ST_LOADING; g_loadT = 0; }
            else if (pick == 1) openMenu();
            break;
        }
        }
        // default button of a panel when it opens (Close on Tasks, the first one elsewhere)
        if (state != g_lastState) {
            g_panelSel = state == ST_TASKS ? 1 : 0;
            g_lastState = state;
        }
        SDL_GL_SwapWindow(win);
    }
    return 0;
}
