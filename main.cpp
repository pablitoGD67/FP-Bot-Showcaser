#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cctype>
#include <string>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <deque>

using namespace geode::prelude;

static constexpr int FPS_LIST[5] = {60, 90, 120, 240, 360};   // 90 = rango +60 -120
static constexpr int PRE = 14;    // pasos antes del click donde se guarda el checkpoint
static constexpr int MAXW = 10;   // ventana maxima que se mide (en pasos)

struct Click {
    int frame;      // paso de fisica desde el inicio del nivel
    bool down;
    int button;
    bool p1;
    int fp = 0;     // 0 = normal, si no: Hz/fps a los que es frame perfect
    int win = 0;    // ventana medida en pasos (0 = sin analizar)
};

struct BotState {
    enum Mode { Idle, Recording, Playing } mode = Idle;
    std::vector<Click> clicks;
    size_t next = 0;
    bool injecting = false;
    bool analyzing = false;
    bool stepping = false;
    int offset = 0;       // desfase del replay en pasos (lo calibra Analizar)
    std::string levelKey;
    int levelId = 0;
    std::string levelName;
    std::string recFile;  // archivo .fpr de la grabacion actual
};
static BotState g;

static CCLabelBMFont* g_fp = nullptr;
static CCLabelBMFont* g_status = nullptr;

static int getTps() { return (int)Mod::get()->getSettingValue<int64_t>("tps"); }

// ---- Ajustes (cache) ----
static bool s_trail = false, s_traj = false;
static int s_hbTps = 480;
static float s_trailSec = 1.f;
static void loadSettings() {
    s_trail = Mod::get()->getSettingValue<bool>("hitbox-trail");
    s_traj = Mod::get()->getSettingValue<bool>("show-trajectory");
    s_hbTps = (int)Mod::get()->getSettingValue<int64_t>("hitbox-tps");
    s_trailSec = (float)Mod::get()->getSettingValue<double>("hitbox-trail-seconds");
}

// ---- Trayectoria (ruta precalculada con "Analizar") ----
static std::vector<CCPoint> g_path;
static CCDrawNode* g_trajNode = nullptr;

static void loadPath() {
    g_path.clear();
    if (g.levelKey.empty()) return;
    auto str = Mod::get()->getSavedValue<std::string>("path-" + g.levelKey, "");
    size_t pos = 0;
    while (pos < str.size()) {
        auto e = str.find(';', pos);
        if (e == std::string::npos) break;
        float x, y;
        if (std::sscanf(str.substr(pos, e - pos).c_str(), "%f,%f", &x, &y) == 2) g_path.push_back(CCPoint(x, y));
        pos = e + 1;
    }
}

static void savePath() {
    if (g.levelKey.empty()) return;
    std::string str;
    for (auto& pt : g_path) str += fmt::format("{:.1f},{:.1f};", pt.x, pt.y);
    Mod::get()->setSavedValue<std::string>("path-" + g.levelKey, str);
}

static void rebuildTrajectory(PlayLayer* pl) {
    if (g_trajNode && g_trajNode->getParent()) g_trajNode->removeFromParent();
    g_trajNode = nullptr;
    if (!pl || !s_traj || g_path.size() < 2 || !pl->m_objectLayer) return;
    auto n = CCDrawNode::create();
    for (size_t i = 1; i < g_path.size(); i++) {
        n->drawSegment(g_path[i - 1], g_path[i], 0.6f, ccc4f(0.2f, 1.f, 0.3f, 0.85f));
    }
    pl->m_objectLayer->addChild(n, 900);
    g_trajNode = n;
}

$on_mod(Loaded) {
    loadSettings();
    listenForSettingChanges<bool>("hitbox-trail", [](bool v) { s_trail = v; });
    listenForSettingChanges<bool>("show-trajectory", [](bool v) {
        s_traj = v;
        if (auto pl = PlayLayer::get()) rebuildTrajectory(pl);
    });
    listenForSettingChanges<int64_t>("hitbox-tps", [](int64_t v) { s_hbTps = (int)v; });
    listenForSettingChanges<double>("hitbox-trail-seconds", [](double v) { s_trailSec = (float)v; });
    listenForSettingChanges<double>("speedhack", [](double v) {
        if (PlayLayer::get()) CCDirector::get()->getScheduler()->setTimeScale(std::max(0.05f, (float)v));
    });
}

// ---- Trail de hitboxes por paso de fisica (con pasos intermedios hasta hitbox-tps) ----
struct Trail {
    std::deque<CCDrawNode*> chunks;
    CCDrawNode* cur = nullptr;
    int inCur = 0;
    CCRect last1, last2;
    bool have1 = false, have2 = false;
    int lastFrame = -1;
};
static Trail g_trail;

static void trailReset() {
    for (auto n : g_trail.chunks) if (n && n->getParent()) n->removeFromParent();
    g_trail = Trail();
}

static void drawRectOn(CCDrawNode* n, CCRect const& r, ccColor4F col) {
    CCPoint v[4] = {
        CCPoint(r.getMinX(), r.getMinY()), CCPoint(r.getMaxX(), r.getMinY()),
        CCPoint(r.getMaxX(), r.getMaxY()), CCPoint(r.getMinX(), r.getMaxY())
    };
    n->drawPolygon(v, 4, ccc4f(0.f, 0.f, 0.f, 0.f), 0.4f, col);
}

static CCRect lerpRect(CCRect const& a, CCRect const& b, float t) {
    return CCRect(
        a.origin.x + (b.origin.x - a.origin.x) * t,
        a.origin.y + (b.origin.y - a.origin.y) * t,
        a.size.width + (b.size.width - a.size.width) * t,
        a.size.height + (b.size.height - a.size.height) * t);
}

static void trailEmit(GJBaseGameLayer* gl, CCRect& last, bool& have, CCRect const& now, ccColor4F col) {
    int tps = getTps();
    int subs = std::max(1, (int)std::lround((double)s_hbTps / std::max(1, tps)));
    int perChunk = 24 * subs;
    int maxChunks = (int)(s_trailSec * tps * subs / perChunk) + 2;
    int n = have ? subs : 1;
    for (int j = 1; j <= n; j++) {
        CCRect r = have ? lerpRect(last, now, (float)j / subs) : now;
        if (!g_trail.cur || g_trail.inCur >= perChunk) {
            auto node = CCDrawNode::create();
            gl->m_objectLayer->addChild(node, 1000);
            g_trail.chunks.push_back(node);
            g_trail.cur = node;
            g_trail.inCur = 0;
            while ((int)g_trail.chunks.size() > maxChunks) {
                g_trail.chunks.front()->removeFromParent();
                g_trail.chunks.pop_front();
            }
        }
        drawRectOn(g_trail.cur, r, col);
        g_trail.inCur++;
    }
    last = now;
    have = true;
}

static void trailCapture(GJBaseGameLayer* gl, bool isHalfTick) {
    if (!s_trail || isHalfTick || !PlayLayer::get()) return;
    if (!gl->m_objectLayer || !gl->m_player1) return;
    int frame = static_cast<int>(gl->m_gameState.m_currentProgress);
    if (frame == g_trail.lastFrame) return;
    g_trail.lastFrame = frame;
    trailEmit(gl, g_trail.last1, g_trail.have1, gl->m_player1->getObjectRect(), ccc4f(1.f, 0.25f, 0.25f, 0.9f));
    if (gl->m_player2 && gl->m_player2->isVisible()) {
        trailEmit(gl, g_trail.last2, g_trail.have2, gl->m_player2->getObjectRect(), ccc4f(0.3f, 0.5f, 1.f, 0.9f));
    }
}

// Ventana (en pasos) -> frecuencia FP, igual que la tabla del "Frame Window Counter"
static int hzClass(int N, int tps) {
    if (N < 1) N = 1;
    if (N > 10) return 0;
    int n = N;
    if (n >= 5 && (n & 1)) n++;   // 5-6 -> 40Hz, 7-8 -> 30Hz, 9-10 -> 24Hz
    return (int)std::lround(tps / (double)n);
}

// Categoria segun el fps minimo que exige el click (los rangos evitan "FP impostores")
static int fpCat(int hz) {
    if (hz <= 60) return 0;     // 60 fps (o menos)
    if (hz < 120) return 1;     // +60 -120
    if (hz == 120) return 2;
    if (hz < 240) return 3;     // +120 -240
    if (hz == 240) return 4;
    if (hz < 360) return 5;     // +240 -360
    return 6;                   // 360 o mas
}
static std::string fpCatText(int cat) {
    switch (cat) {
        case 0: return "a 60 fps";
        case 1: return "+60 fps -120 fps";
        case 2: return "a 120 fps";
        case 3: return "+120 fps -240 fps";
        case 4: return "a 240 fps";
        case 5: return "+240 fps -360 fps";
        default: return "a 360 fps";
    }
}

// ---- Guardado en disco (una grabacion por nivel) ----
static std::string recKey(GJGameLevel* l) {
    return fmt::format("rec-{}-{}", (int)l->m_levelID, std::string(l->m_levelName));
}

// ---- Archivos de grabacion (.fpr): se escriben al instante y no se borran ----
static std::filesystem::path replaysDir() {
    auto d = Mod::get()->getSaveDir() / "replays";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

static std::string sanitizeName(std::string n) {
    for (auto& ch : n) if (!std::isalnum((unsigned char)ch)) ch = '_';
    return n.empty() ? std::string("nivel") : n;
}

static void saveRec() {
    if (g.clicks.empty() || g.levelName.empty()) return;
    if (g.recFile.empty()) {
        char ts[32];
        std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&t));
        g.recFile = (replaysDir() / fmt::format("{}_{}_{}.fpr", sanitizeName(g.levelName), g.levelId, ts)).string();
    }
    std::ofstream f(g.recFile, std::ios::trunc);
    if (!f) return;
    f << "FPR1\n" << "id=" << g.levelId << "\n" << "name=" << g.levelName << "\n"
      << "tps=" << getTps() << "\n" << "offset=" << g.offset << "\n" << "clicks\n";
    for (auto& c : g.clicks) {
        f << c.frame << ',' << (c.down ? 1 : 0) << ',' << c.button << ',' << (c.p1 ? 1 : 0)
          << ',' << c.fp << ',' << c.win << '\n';
    }
}

static bool readRecFile(std::filesystem::path const& path, int& id, std::string& name, int& offset, std::vector<Click>* out) {
    std::ifstream f(path);
    std::string line;
    if (!f || !std::getline(f, line) || line.rfind("FPR1", 0) != 0) return false;
    id = 0; name.clear(); offset = 0;
    while (std::getline(f, line)) {
        if (line == "clicks") break;
        if (line.rfind("id=", 0) == 0) id = std::atoi(line.c_str() + 3);
        else if (line.rfind("name=", 0) == 0) name = line.substr(5);
        else if (line.rfind("offset=", 0) == 0) offset = std::atoi(line.c_str() + 7);
    }
    if (out) {
        while (std::getline(f, line)) {
            int a, b, c, d, e, w = 0;
            if (std::sscanf(line.c_str(), "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &e, &w) >= 5) {
                out->push_back(Click{a, b != 0, c, d != 0, e, w});
            }
        }
    }
    return true;
}

// carga la grabacion mas reciente de este nivel
static void loadRec() {
    g.clicks.clear();
    g.offset = 0;
    g.recFile.clear();
    if (g.levelName.empty()) return;
    std::filesystem::path best;
    std::filesystem::file_time_type bestT{};
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(replaysDir(), ec)) {
        if (e.path().extension() != ".fpr") continue;
        int id, off;
        std::string name;
        if (!readRecFile(e.path(), id, name, off, nullptr)) continue;
        if (id != g.levelId || name != g.levelName) continue;
        std::error_code e2;
        auto t = std::filesystem::last_write_time(e.path(), e2);
        if (best.empty() || t > bestT) { best = e.path(); bestT = t; }
    }
    if (!best.empty()) {
        int id, off;
        std::string name;
        std::vector<Click> v;
        if (readRecFile(best, id, name, off, &v) && !v.empty()) {
            g.clicks = v;
            g.offset = off;
            g.recFile = best.string();
            return;
        }
    }
    // migracion: grabaciones de versiones anteriores (guardadas dentro del mod)
    auto old = Mod::get()->getSavedValue<std::string>(g.levelKey, "");
    size_t pos = 0;
    while (pos < old.size()) {
        auto end = old.find(';', pos);
        if (end == std::string::npos) break;
        auto item = old.substr(pos, end - pos);
        pos = end + 1;
        int a, b, c, d, e, w = 0;
        if (std::sscanf(item.c_str(), "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &e, &w) >= 5) {
            g.clicks.push_back(Click{a, b != 0, c, d != 0, e, w});
        }
    }
    if (!g.clicks.empty()) {
        g.offset = (int)Mod::get()->getSavedValue<int64_t>("calib-" + g.levelKey, 0);
        saveRec();
    }
}

// ---- Lector de replays .gdr2 (Eclipse / formato GDR2) ----
struct Gdr2 {
    bool ok = false;
    int tps = 0;
    std::string level;
    std::vector<Click> clicks;
};

static bool rdVar(std::vector<uint8_t> const& d, size_t& p, uint64_t& out) {
    out = 0;
    int shift = 0;
    while (p < d.size() && shift < 63) {
        uint8_t b = d[p++];
        out |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}

static Gdr2 parseGdr2(std::filesystem::path const& path) {
    Gdr2 r;
    std::ifstream f(path, std::ios::binary);
    if (!f) return r;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 8 || d[0] != 'G' || d[1] != 'D' || d[2] != 'R') return r;

    // Los inputs son un vector de varints [n, v1..vn] que termina justo al final del archivo.
    // Cada valor = (pasos desde el input anterior << 1) | down
    size_t bestQ = 0;
    uint64_t bestN = 0;
    for (size_t q = 3; q + 1 < d.size(); q++) {
        size_t p = q;
        uint64_t n;
        if (!rdVar(d, p, n) || n == 0 || n > 1000000) continue;
        bool good = true;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t v;
            if (!rdVar(d, p, v)) { good = false; break; }
        }
        if (good && p == d.size() && n > bestN) { bestN = n; bestQ = q; }
    }
    if (!bestN) return r;

    size_t p = bestQ;
    uint64_t n;
    rdVar(d, p, n);
    int frame = 0;
    bool held = false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t v;
        rdVar(d, p, v);
        frame += (int)(v >> 1);
        bool down = (v & 1) != 0;
        if (down) { r.clicks.push_back({frame, true, 1, true, 0, 0}); held = true; }
        else if (held) { r.clicks.push_back({frame, false, 1, true, 0, 0}); held = false; }
    }

    // TPS: doble big-endian entero entre 30 y 1000 en la cabecera
    for (size_t i = 3; i + 8 <= bestQ; i++) {
        uint64_t bits = 0;
        for (int k = 0; k < 8; k++) bits = (bits << 8) | d[i + k];
        double v;
        std::memcpy(&v, &bits, 8);
        if (std::isfinite(v) && v >= 30 && v <= 1000 && v == std::floor(v)) { r.tps = (int)v; break; }
    }

    // nombre del nivel: ultima cadena imprimible justo antes de los inputs
    for (size_t s0 = 3; s0 < bestQ; s0++) {
        size_t pp = s0;
        uint64_t len;
        if (!rdVar(d, pp, len) || len < 1 || len > 80 || pp + len > bestQ) continue;
        size_t end = pp + len;
        if (bestQ - end > 8) continue;
        bool printable = true;
        for (size_t k = pp; k < end; k++) if (d[k] < 0x20 || d[k] > 0x7e) { printable = false; break; }
        if (printable && len > r.level.size()) r.level.assign((char const*)&d[pp], len);
    }
    r.ok = !r.clicks.empty();
    return r;
}

static std::vector<std::filesystem::path> findReplays() {
    std::vector<std::filesystem::path> out;
    std::vector<std::filesystem::path> roots = {Mod::get()->getSaveDir(), dirs::getGeodeDir()};
    for (auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator endIt;
        for (; !ec && it != endIt; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2) && it->path().extension() == ".gdr2") {
                if (std::find(out.begin(), out.end(), it->path()) == out.end()) out.push_back(it->path());
            }
        }
    }
    return out;
}

// importa el .gdr2 mas reciente (prefiere el del mismo nivel)
static bool importReplay(std::string const& levelName, std::string& msg) {
    auto files = findReplays();
    if (files.empty()) { msg = "No encontre ningun .gdr2 en la carpeta de Geode"; return false; }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) {
        std::error_code e1, e2;
        return std::filesystem::last_write_time(a, e1) > std::filesystem::last_write_time(b, e2);
    });
    Gdr2 best;
    std::filesystem::path bestPath;
    for (auto& f : files) {
        auto r = parseGdr2(f);
        if (!r.ok) continue;
        if (r.level == levelName) { best = r; bestPath = f; break; }
        if (!best.ok) { best = r; bestPath = f; }
    }
    if (!best.ok) { msg = "Ningun .gdr2 se pudo leer"; return false; }
    g.clicks = best.clicks;
    g.next = 0;
    g.offset = 0;
    Mod::get()->setSavedValue<int64_t>("calib-" + g.levelKey, 0);
    if (best.tps > 0) Mod::get()->setSettingValue<int64_t>("tps", best.tps);
    g.recFile.clear();   // el replay importado se guarda como archivo nuevo
    saveRec();
    int downs = 0;
    for (auto& c : g.clicks) if (c.down) downs++;
    msg = fmt::format("Importado {} ({} clicks, {} TPS)", bestPath.filename().string(), downs, best.tps ? best.tps : getTps());
    if (best.level != levelName && !best.level.empty()) msg += fmt::format(" - ojo: es de '{}'", best.level);
    return true;
}

static void refreshLabels() {
    if (g_fp) {
        std::map<int,int, std::greater<int>> m;
        for (auto& c : g.clicks) if (c.down && c.fp) m[fpCat(c.fp)]++;
        std::string t;
        if (m.empty()) t = "FP: 0";
        for (auto& [cat, n] : m) {
            if (!t.empty()) t += "\n";
            t += fmt::format("FP {}: {}", fpCatText(cat), n);
        }
        g_fp->setString(t.c_str());
    }
    if (g_status && !g.analyzing) {
        char const* s = g.mode == BotState::Recording ? "GRABANDO"
                      : g.mode == BotState::Playing ? "REPRODUCIENDO" : "";
        g_status->setString(s);
    }
}

// notificacion arriba a la derecha cuando un click es FP (se apilan hacia abajo)
static std::vector<Ref<CCLabelBMFont>> s_notes;
static void popup(std::string const& text) {
    auto pl = PlayLayer::get();
    if (!pl) return;
    s_notes.erase(std::remove_if(s_notes.begin(), s_notes.end(),
        [](Ref<CCLabelBMFont>& n) { return !n->getParent(); }), s_notes.end());
    auto win = CCDirector::get()->getWinSize();
    auto l = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    l->setScale(0.42f);
    l->setAnchorPoint({1.f, 0.5f});
    l->setPosition({win.width - 18.f, win.height - 50.f - (float)s_notes.size() * 22.f});
    l->setOpacity(0);
    pl->addChild(l, 200);
    s_notes.push_back(Ref<CCLabelBMFont>(l));
    l->runAction(CCSequence::create(
        CCFadeIn::create(0.15f), CCDelayTime::create(1.3f), CCFadeOut::create(0.4f),
        CCRemoveSelf::create(), nullptr));
}

// ---------- Motor de analisis ----------
struct An {
    bool on = false;
    PlayLayer* pl = nullptr;
    CCNode* node = nullptr;
    int tps = 240;
    std::vector<Click> base;          // eventos grabados
    std::vector<Click> ev;            // eventos de la prueba actual
    size_t ei = 0;
    std::vector<size_t> downIdx;      // indices de los clicks "down" a analizar
    std::map<size_t,int> results;     // idx -> ventana
    size_t k = 0;
    enum St { Calib, Advance, Trials, Final } st = Calib;
    std::vector<Click> base0;   // eventos sin desfase
    std::vector<int> offsets = {0, -1, -2, -3, 1, 2, -4};
    size_t oi = 0;
    int offset = 0, bestFrame = -1, bestOff = 0;
    int stall = 0;
    bool yield = false;
    BotState::Mode prevMode = BotState::Idle;   // modo del bot antes de analizar
    Ref<CheckpointObject> cp;
    int cpFrame = 0, frame = 0, goal = 0, lastFrame = 0;
    bool died = false;
    int dir = -1, s = -1, a = 0, b = 0;
    long long steps = 0;
    std::map<int, std::pair<float,float>> path;   // frame -> posicion del jugador 1
};
static An an;

static void stopAnalysis(bool ok, std::string const& why = "");

static void buildEvents(size_t di, int shift) {
    an.ev = an.base;
    if (shift == 0) return;
    int upIdx = -1;
    for (size_t j = di + 1; j < an.ev.size(); j++) {
        if (!an.ev[j].down && an.ev[j].button == an.ev[di].button && an.ev[j].p1 == an.ev[di].p1) { upIdx = (int)j; break; }
    }
    an.ev[di].frame = std::max(0, an.ev[di].frame + shift);
    if (upIdx >= 0) an.ev[upIdx].frame = std::max(0, an.ev[upIdx].frame + shift);
    std::stable_sort(an.ev.begin(), an.ev.end(), [](Click const& x, Click const& y) { return x.frame < y.frame; });
}

static void applyOffset(int o) {
    an.offset = o;
    an.base = an.base0;
    for (auto& c : an.base) c.frame = std::max(0, c.frame + o);
}

static void prime(PlayLayer* pl, int F0) {
    an.frame = F0; an.ei = 0; an.died = false;
    std::map<std::pair<int,bool>, bool> held;
    while (an.ei < an.ev.size() && an.ev[an.ei].frame < F0) {
        auto& c = an.ev[an.ei++];
        held[{c.button, c.p1}] = c.down;
    }
    for (auto& [key, h] : held) {
        if (h) {
            g.injecting = true;
            pl->handleButton(true, key.first, key.second);
            g.injecting = false;
        }
    }
}

static bool stepOne(PlayLayer* pl) {
    int before = static_cast<int>(pl->m_gameState.m_currentProgress);
    for (int t = 0; t < 8; t++) {
        while (an.ei < an.ev.size() && an.ev[an.ei].frame <= an.frame) {
            auto& c = an.ev[an.ei++];
            g.injecting = true;
            pl->handleButton(c.down, c.button, c.p1);
            g.injecting = false;
        }
        g.stepping = true;
        pl->update(1.f / an.tps);
        g.stepping = false;
        int after = static_cast<int>(pl->m_gameState.m_currentProgress);
        if (after != before) {
            an.frame += after - before;
            if (an.st != An::Trials && pl->m_player1) {
                an.path[an.frame] = {(float)pl->m_player1->getPositionX(), (float)pl->m_player1->getPositionY()};
            }
            return true;
        }
    }
    return false;
}

static void startTrial(PlayLayer* pl) {
    size_t di = an.downIdx[an.k];
    buildEvents(di, an.s);
    pl->loadFromCheckpoint(an.cp);
    prime(pl, an.cpFrame);
    an.goal = std::min(an.base[di].frame + an.tps, an.lastFrame);
}

static void finishAnalysis() {
    int found = 0;
    for (auto& [idx, N] : an.results) {
        g.clicks[idx].win = N;
        g.clicks[idx].fp = hzClass(N, an.tps);
        if (g.clicks[idx].fp) found++;
    }
    saveRec();
    g_path.clear();
    for (auto& [fr, xy] : an.path) g_path.push_back(CCPoint(xy.first, xy.second));
    savePath();
    auto plDone = an.pl;
    stopAnalysis(true);
    if (plDone && PlayLayer::get() == plDone) rebuildTrajectory(plDone);
    Notification::create(fmt::format("Analisis listo: {} clicks FP (desfase {})", found, an.offset), NotificationIcon::Success)->show();
}

// una micro-tarea de la maquina de estados; false = terminado/abortado
static bool tickInner(PlayLayer* pl);
static bool tick(PlayLayer* pl) {
    if (++an.steps > 30000000) { stopAnalysis(false, "demasiados pasos"); return false; }
    return tickInner(pl);
}

static bool tickInner(PlayLayer* pl) {

    if (an.st == An::Calib) {
        // busca el desfase con el que el replay SI se reproduce sin morir
        if (an.died || an.frame >= an.goal) {
            int cand = an.offsets[an.oi];
            if (an.frame > an.bestFrame) { an.bestFrame = an.frame; an.bestOff = cand; }
            bool ok = !an.died;
            if (!ok && an.oi + 1 < an.offsets.size()) {
                an.oi++;
                applyOffset(an.offsets[an.oi]);
                pl->resetLevel();
                an.ev = an.base;
                prime(pl, 0);
                return true;
            }
            int use = ok ? cand : an.bestOff;
            if (!ok) {
                Notification::create(fmt::format("Calibracion: el replay muere en el paso {} (mejor desfase {})", an.bestFrame, use), NotificationIcon::Warning)->show();
            }
            applyOffset(use);
            g.offset = use;
            Mod::get()->setSavedValue<int64_t>("calib-" + g.levelKey, (int64_t)use);
            saveRec();
            pl->resetLevel();
            an.ev = an.base;
            prime(pl, 0);
            an.path.clear();
            an.goal = std::max(0, an.base[an.downIdx[0]].frame - PRE);
            an.st = An::Advance;
            return true;
        }
        if (!stepOne(pl)) {
            if (++an.stall > 180) { stopAnalysis(false, "el juego no avanza (calibracion, paso " + std::to_string(an.frame) + ")"); return false; }
            an.yield = true;
            return true;
        }
        an.stall = 0;
        return true;
    }

    if (an.st == An::Advance) {
        if (an.frame >= an.goal) {
            an.cp = pl->createCheckpoint();
            an.cpFrame = an.frame;
            an.dir = -1; an.s = -1; an.a = 0; an.b = 0;
            an.st = An::Trials;
            startTrial(pl);
            return true;
        }
        if (an.died) { stopAnalysis(false, "el replay muere en el paso " + std::to_string(an.frame) + " antes de medir (desfase " + std::to_string(an.offset) + ")"); return false; }
        if (!stepOne(pl)) {
            if (++an.stall > 180) { stopAnalysis(false, "el juego no avanza (paso " + std::to_string(an.frame) + ")"); return false; }
            an.yield = true;
            return true;
        }
        an.stall = 0;
        return true;
    }

    if (an.st == An::Final) {
        if (an.frame >= an.goal || an.died) { finishAnalysis(); return false; }
        if (!stepOne(pl)) {
            if (++an.stall > 180) { finishAnalysis(); return false; }
            an.yield = true;
            return true;
        }
        an.stall = 0;
        return true;
    }

    // Trials
    if (an.frame < an.goal && !an.died) {
        if (!stepOne(pl)) {
            if (++an.stall > 180) { stopAnalysis(false, "el juego no avanza (prueba, paso " + std::to_string(an.frame) + ")"); return false; }
            an.yield = true;
            return true;
        }
        an.stall = 0;
        return true;
    }
    bool survived = !an.died;
    if (survived) {
        (an.dir < 0 ? an.a : an.b)++;
        if (std::abs(an.s) < MAXW) { an.s += an.dir; startTrial(pl); return true; }
    }
    if (an.dir < 0) { an.dir = 1; an.s = 1; startTrial(pl); return true; }

    // fin de las pruebas de este click
    an.results[an.downIdx[an.k]] = an.a + an.b + 1;
    an.ev = an.base;
    pl->loadFromCheckpoint(an.cp);
    prime(pl, an.cpFrame);
    an.k++;
    if (an.k >= an.downIdx.size()) { an.st = An::Final; an.goal = an.lastFrame; return true; }
    an.goal = std::max(an.cpFrame, an.base[an.downIdx[an.k]].frame - PRE);
    an.st = An::Advance;
    return true;
}

class AnalysisNode : public CCNode {
public:
    static AnalysisNode* create() {
        auto n = new AnalysisNode();
        if (n && n->init()) { n->autorelease(); return n; }
        delete n;
        return nullptr;
    }
    void update(float) override {
        if (!an.on) { this->removeFromParent(); return; }
        auto pl = PlayLayer::get();
        if (!pl || pl != an.pl) { stopAnalysis(false, "saliste del nivel"); return; }
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
            if (!tick(pl)) break;
            if (an.yield) { an.yield = false; break; }
        }
        if (an.on && g_status) {
            g_status->setString(fmt::format("ANALIZANDO {}/{}", an.k + 1, an.downIdx.size()).c_str());
        }
    }
};

static void stopAnalysis(bool ok, std::string const& why) {
    auto pl = an.pl;
    bool wasOn = an.on;
    an.on = false;
    g.analyzing = false;
    g.stepping = false;
    // volver al modo en que estaba el bot (Replay se conserva; Grabar no, porque se borraria)
    if (wasOn) g.mode = (an.prevMode == BotState::Playing) ? BotState::Playing : BotState::Idle;
    an.cp = nullptr;
    if (an.node && an.node->getParent()) an.node->removeFromParent();
    an.node = nullptr;
    an.pl = nullptr;
    if (!wasOn) return;
    if (!ok) {
        log::warn("[FP Bot] analisis fallido: {}", why);
        Notification::create(why.empty() ? std::string("Analisis cancelado") : "Analisis fallido: " + why, NotificationIcon::Error)->show();
    }
    if (pl && PlayLayer::get() == pl) pl->resetLevel();
    refreshLabels();
}

static bool startAnalysis(PlayLayer* pl) {
    if (g.clicks.empty()) {
        Notification::create("No hay grabacion para analizar", NotificationIcon::Error)->show();
        return false;
    }
    an = An();
    an.tps = getTps();
    an.base = g.clicks;
    std::stable_sort(an.base.begin(), an.base.end(), [](Click const& x, Click const& y) { return x.frame < y.frame; });
    g.clicks = an.base;   // mantener el mismo orden que usa el analisis
    for (size_t i = 0; i < an.base.size(); i++) if (an.base[i].down) an.downIdx.push_back(i);
    if (an.downIdx.empty()) return false;
    an.lastFrame = an.base.back().frame + an.tps / 2;
    for (auto& c : g.clicks) { c.fp = 0; c.win = 0; }
    an.pl = pl;
    an.on = true;
    g.analyzing = true;
    an.prevMode = g.mode;
    g.mode = BotState::Idle;

    an.base0 = an.base;
    pl->resetLevel();
    applyOffset(0);
    an.ev = an.base;
    prime(pl, 0);
    an.goal = an.base.back().frame + an.tps / 4;
    an.st = An::Calib;

    auto node = AnalysisNode::create();
    node->scheduleUpdate();
    CCDirector::get()->getRunningScene()->addChild(node);
    an.node = node;
    return true;
}

// ---- Captura / reproduccion de inputs ----
class $modify(FPGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        if (g.analyzing && !g.stepping) return;   // durante el analisis solo avanza nuestro bucle
        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g.mode == BotState::Recording && !g.injecting && !g.analyzing) {
            g.clicks.push_back({static_cast<int>(m_gameState.m_currentProgress), down, button, isPlayer1, 0, 0});
            if (!down) saveRec();
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!g.analyzing) trailCapture(this, isHalfTick);
        if (g.mode == BotState::Playing && !g.analyzing) {
            int now = static_cast<int>(m_gameState.m_currentProgress);
            while (g.next < g.clicks.size() && g.clicks[g.next].frame + g.offset <= now) {
                auto& c = g.clicks[g.next++];
                g.injecting = true;
                GJBaseGameLayer::handleButton(c.down, c.button, c.p1);
                g.injecting = false;
                if (c.down && c.fp && Mod::get()->getSettingValue<bool>("fp-notification")) popup("FP detectado " + fpCatText(fpCat(c.fp)));
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// al reaparecer en un checkpoint (modo practica): conserva lo grabado hasta ese frame
static void truncateClicks(int now) {
    g.clicks.erase(std::remove_if(g.clicks.begin(), g.clicks.end(),
        [now](Click const& c) { return c.frame >= now; }), g.clicks.end());
    std::map<std::pair<int,bool>, bool> held;
    for (auto& c : g.clicks) held[{c.button, c.p1}] = c.down;
    for (auto& [key, h] : held) {
        if (h) g.clicks.push_back({now, false, key.first, key.second, 0, 0});   // soltar el boton en el checkpoint
    }
}

// ---- UI en el nivel ----
class $modify(FPPlayLayer, PlayLayer) {
    struct Fields {
        ~Fields() {
            if (g.mode == BotState::Recording) saveRec();
            g.mode = BotState::Idle;
            g.analyzing = false;
            g.stepping = false;
            an.on = false;
            an.cp = nullptr;
            an.node = nullptr;
            an.pl = nullptr;
            g_fp = nullptr;
            g_status = nullptr;
            s_notes.clear();
            g_trail = Trail();      // los nodos mueren con el nivel
            g_trajNode = nullptr;
            if (auto d = CCDirector::get()) d->getScheduler()->setTimeScale(1.f);
        }
    };

    bool init(GJGameLevel* lvl, bool a, bool b) {
        g.mode = BotState::Idle;
        g.analyzing = false;
        an.on = false;
        g.next = 0;
        g.levelKey = recKey(lvl);
        g.levelId = (int)lvl->m_levelID;
        g.levelName = std::string(lvl->m_levelName);
        loadRec();
        loadPath();
        loadSettings();
        g_fp = nullptr;
        g_status = nullptr;
        s_notes.clear();
        g_trail = Trail();
        g_trajNode = nullptr;

        if (!PlayLayer::init(lvl, a, b)) return false;
        auto win = CCDirector::get()->getWinSize();

        g_status = CCLabelBMFont::create("", "goldFont.fnt");
        g_status->setScale(0.4f);
        g_status->setAnchorPoint({0.f, 1.f});
        g_status->setPosition({6.f, win.height - 64.f});
        g_status->setZOrder(100);
        this->addChild(g_status);

        g_fp = CCLabelBMFont::create("", "chatFont.fnt");
        g_fp->setScale(0.5f);
        g_fp->setAnchorPoint({0.f, 1.f});
        g_fp->setPosition({6.f, win.height - 76.f});
        g_fp->setZOrder(100);
        this->addChild(g_fp);

        // botones manuales (opcionales, desactivados por defecto)
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setZOrder(1000);
        menu->setTouchPriority(-600);
        menu->setVisible(Mod::get()->getSettingValue<bool>("manual-buttons"));
        for (int i = 0; i < 5; i++) {
            auto spr = ButtonSprite::create((FPS_LIST[i] == 90 ? std::string("+60-120") : std::to_string(FPS_LIST[i])).c_str(), 40, true, "bigFont.fnt", "GJ_button_04.png", 20.f, 0.5f);
            spr->setOpacity(150);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FPPlayLayer::onTag));
            btn->setTag(FPS_LIST[i]);
            btn->setPosition({win.width - 30.f, 20.f + i * 24.f});
            menu->addChild(btn);
        }
        this->addChild(menu);
        refreshLabels();
        rebuildTrajectory(this);
        CCDirector::get()->getScheduler()->setTimeScale(
            std::max(0.05f, (float)Mod::get()->getSettingValue<double>("speedhack")));
        return true;
    }

    void onTag(CCObject* o) {
        int fps = static_cast<CCNode*>(o)->getTag();
        for (int i = (int)g.clicks.size() - 1; i >= 0; i--) {
            if (g.clicks[i].down) { g.clicks[i].fp = fps; break; }
        }
        saveRec();
        refreshLabels();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        int now = static_cast<int>(m_gameState.m_currentProgress);
        if (g.mode == BotState::Recording && !g.analyzing) {
            if (m_isPracticeMode && now > 0) { truncateClicks(now); saveRec(); }   // practica: sigue desde el checkpoint
            else g.clicks.clear();                                  // normal: nueva grabacion desde 0
        }
        trailReset();
        g.next = 0;
        if (g.mode == BotState::Playing && !g.analyzing && now > 0) {
            while (g.next < g.clicks.size() && g.clicks[g.next].frame + g.offset < now) g.next++;
        }
        refreshLabels();
    }

    // durante el analisis la muerte y la meta no deben ejecutarse de verdad
    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (g.analyzing) { an.died = true; return; }
        PlayLayer::destroyPlayer(p, o);
    }

    void levelComplete() {
        if (g.analyzing) return;
        PlayLayer::levelComplete();
        if (g.mode == BotState::Recording) {
            saveRec();
            g.mode = BotState::Idle;
            refreshLabels();
        }
    }
};

// ---- Controles en el menu de pausa ----
class $modify(FPPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setZOrder(1000);
        menu->setTouchPriority(-600);

        auto mk = [&](char const* t, SEL_MenuHandler cb, float y) {
            auto spr = ButtonSprite::create(t, 90, true, "goldFont.fnt", "GJ_button_01.png", 28.f, 0.6f);
            auto b = CCMenuItemSpriteExtra::create(spr, this, cb);
            b->setPosition({win.width - 60.f, y});
            menu->addChild(b);
        };
        mk("Grabar", menu_selector(FPPauseLayer::onRec), win.height - 40.f);
        mk("Parar", menu_selector(FPPauseLayer::onStop), win.height - 72.f);
        mk("Replay", menu_selector(FPPauseLayer::onPlay), win.height - 104.f);
        mk("Analizar", menu_selector(FPPauseLayer::onAnalyze), win.height - 136.f);
        mk("Importar", menu_selector(FPPauseLayer::onImport), win.height - 168.f);
        mk("Ajustes", menu_selector(FPPauseLayer::onSettings), win.height - 200.f);
        this->addChild(menu);
    }
    void onRec(CCObject*) {
        g.mode = BotState::Recording; g.clicks.clear(); g.next = 0; g.recFile.clear();
        Notification::create("Grabando: reinicia el nivel (en practica sigue desde el checkpoint)", NotificationIcon::Info)->show();
        refreshLabels();
    }
    void onStop(CCObject*) {
        if (g.mode == BotState::Recording) {
            saveRec();
            log::info("[FP Bot] grabacion guardada en {}", g.recFile);
            Notification::create("Guardado: " + std::filesystem::path(g.recFile).filename().string(), NotificationIcon::Success)->show();
        }
        g.mode = BotState::Idle;
        refreshLabels();
    }
    void onPlay(CCObject*) {
        if (g.clicks.empty()) {
            Notification::create("No hay grabacion", NotificationIcon::Error)->show();
            return;
        }
        g.mode = BotState::Playing; g.next = 0;
        Notification::create("Replay: reinicia el nivel", NotificationIcon::Info)->show();
        refreshLabels();
    }
    void onSettings(CCObject*) {
        openSettingsPopup(Mod::get(), false);
    }
    void onImport(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        std::string msg;
        bool ok = importReplay(std::string(pl->m_level->m_levelName), msg);
        Notification::create(msg, ok ? NotificationIcon::Success : NotificationIcon::Error)->show();
        refreshLabels();
    }
    void onAnalyze(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (pl->m_isPracticeMode) {
            Notification::create("Desactiva el modo practica para analizar", NotificationIcon::Error)->show();
            return;
        }
        if (!startAnalysis(pl)) return;
        Notification::create("Analizando frames perfect...", NotificationIcon::Loading)->show();
        this->onResume(nullptr);
    }
};
