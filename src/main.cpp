#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <vector>
#include <map>

using namespace geode::prelude;

static constexpr int FPS_LIST[5] = {60, 90, 120, 240, 360};

struct Click {
    int frame;      // paso de fisica desde el inicio del nivel
    bool down;
    int button;
    bool p1;
    int fp = 0;     // 0 = normal, si no: fps al que es frame perfect
};

struct BotState {
    enum Mode { Idle, Recording, Playing } mode = Idle;
    std::vector<Click> clicks;
    size_t next = 0;      // siguiente click a reproducir
    bool injecting = false;
    std::map<int,int> counters() const {
        std::map<int,int> m;
        for (int f : FPS_LIST) m[f] = 0;
        for (auto& c : clicks) if (c.down && c.fp) m[c.fp]++;
        return m;
    }
};
static BotState g;

static CCLabelBMFont* g_labels[5] = {};
static CCLabelBMFont* g_status = nullptr;

static void refreshLabels() {
    auto m = g.counters();
    for (int i = 0; i < 5; i++) {
        if (!g_labels[i]) continue;
        g_labels[i]->setString(fmt::format("FP a {} fps: {}", FPS_LIST[i], m[FPS_LIST[i]]).c_str());
    }
    if (g_status) {
        char const* s = g.mode == BotState::Recording ? "GRABANDO"
                      : g.mode == BotState::Playing ? "REPRODUCIENDO" : "";
        g_status->setString(s);
    }
}

// ---- Captura / reproduccion de inputs ----
class $modify(FPGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g.mode == BotState::Recording && !g.injecting) {
            g.clicks.push_back({m_gameState.m_currentProgress, down, button, isPlayer1, 0});
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g.mode == BotState::Playing) {
            int now = m_gameState.m_currentProgress;
            while (g.next < g.clicks.size() && g.clicks[g.next].frame <= now) {
                auto& c = g.clicks[g.next++];
                g.injecting = true;
                GJBaseGameLayer::handleButton(c.down, c.button, c.p1);
                g.injecting = false;
            }
            refreshLabels();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---- UI en el nivel ----
class $modify(FPPlayLayer, PlayLayer) {
    bool init(GJGameLevel* lvl, bool a, bool b) {
        if (!PlayLayer::init(lvl, a, b)) return false;
        auto win = CCDirector::get()->getWinSize();

        for (int i = 0; i < 5; i++) {
            auto l = CCLabelBMFont::create("", "chatFont.fnt");
            l->setScale(0.5f);
            l->setAnchorPoint({0.f, 1.f});
            l->setPosition({6.f, win.height - 40.f - i * 12.f});
            l->setZOrder(100);
            this->addChild(l);
            g_labels[i] = l;
        }
        g_status = CCLabelBMFont::create("", "goldFont.fnt");
        g_status->setScale(0.4f);
        g_status->setAnchorPoint({0.f, 1.f});
        g_status->setPosition({6.f, win.height - 28.f});
        g_status->setZOrder(100);
        this->addChild(g_status);

        // Botones para marcar el ultimo click como FP (fase 1, manual)
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setZOrder(1000);
        menu->setTouchPriority(-600);
        for (int i = 0; i < 5; i++) {
            auto spr = ButtonSprite::create(std::to_string(FPS_LIST[i]).c_str(), 40, true, "bigFont.fnt", "GJ_button_04.png", 20.f, 0.5f);
            spr->setOpacity(150);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FPPlayLayer::onTag));
            btn->setTag(FPS_LIST[i]);
            btn->setPosition({win.width - 30.f, 20.f + i * 24.f});
            menu->addChild(btn);
        }
        this->addChild(menu);
        refreshLabels();
        return true;
    }

    void onTag(CCObject* o) {
        int fps = static_cast<CCNode*>(o)->getTag();
        for (int i = (int)g.clicks.size() - 1; i >= 0; i--) {
            if (g.clicks[i].down) { g.clicks[i].fp = fps; break; }
        }
        refreshLabels();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (g.mode == BotState::Recording) {
            // al reiniciar se empieza una grabacion nueva, conservando las marcas solo si no hay clicks
            g.clicks.clear();
        }
        g.next = 0;
        refreshLabels();
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
        this->addChild(menu);
    }
    void onRec(CCObject*) {
        g.mode = BotState::Recording; g.clicks.clear(); g.next = 0;
        Notification::create("Grabando: reinicia el nivel desde 0%", NotificationIcon::Info)->show();
        refreshLabels();
    }
    void onStop(CCObject*) { g.mode = BotState::Idle; refreshLabels(); }
    void onPlay(CCObject*) {
        if (g.clicks.empty()) {
            Notification::create("No hay grabacion", NotificationIcon::Error)->show();
            return;
        }
        g.mode = BotState::Playing; g.next = 0;
        Notification::create("Replay: reinicia el nivel", NotificationIcon::Info)->show();
        refreshLabels();
    }
};
