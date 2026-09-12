#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/LevelListLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/AchievementManager.hpp>
#include <Geode/binding/LevelCell.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/GJLevelList.hpp>
#include <fstream>
#include <set>

using namespace geode::prelude;

// ==================== НАСТРОЙКИ ====================
static bool  g_botEnabled = false;
static float g_baseSpeed = 6.0f;
static float g_currentSpeed = 6.0f;
static int   g_minFPS = 40;
static bool  g_noclipEnabled = true;
static bool  g_autoclickerEnabled = true;
static int   g_autoclickerMinFPS = 35;

// ==================== СОСТОЯНИЕ ====================
enum class BotState { Idle, Collecting, Playing, Waiting };
static BotState g_state = BotState::Idle;

static std::vector<Ref<GJGameLevel>> g_levelQueue;
static size_t g_currentIndex = 0;
static LevelBrowserLayer* g_browser = nullptr;
static Ref<GJSearchObject> g_currentSearch = nullptr;
static Ref<GJLevelList> g_currentList = nullptr;
static bool g_listMode = false;

static float g_waitTimer = 0.0f;
static float g_at100Timer = 0.0f;
static float g_noMoveTimer = 0.0f;
static float g_lastPlayerX = -99999.f;
static float g_levelPlayTime = 0.0f;
static bool  g_isPlatformer = false;
static bool  g_completing = false;
static int   g_downloadRetries = 0;
static int   g_lastDownloadID = 0;
static int   g_inputFrame = 0;
static int   g_sessionCompleted = 0;
static int   g_watchdogSilent = 0;
static int   g_lastPlayedID = 0;

static std::set<int> g_blacklist;

static const float MAX_TIME_CLASSIC     = 55.0f;
static const float MAX_TIME_PLATFORMER  = 10.0f;
static const float FORCE_COMPLETE_PLAT  = 6.0f;
static const float STUCK_100_TIME       = 4.0f;
static const float NO_MOVE_TIME         = 7.0f;
static const float PLATFORMER_SPEED     = 1.0f;
static const float MAX_CLASSIC_SPEED    = 30.0f;
static const float AFTER_COMPLETE_WAIT  = 5.5f;
static const float BETWEEN_LEVELS_WAIT  = 2.8f;

// ==================== ФАЙЛЫ ====================
std::string blacklistPath() {
    return (Mod::get()->getSaveDir() / "blacklist.txt").string();
}
std::string lastCrashPath() {
    return (Mod::get()->getSaveDir() / "last_level.txt").string();
}

void loadBlacklist() {
    g_blacklist.clear();
    std::ifstream f(blacklistPath());
    int id;
    while (f >> id) g_blacklist.insert(id);

    std::ifstream c(lastCrashPath());
    if (c >> id && id > 20) {
        g_blacklist.insert(id);
        log::warn("[AutoGrinder] Safe mode: {} → blacklist", id);
    }
}

void saveBlacklist() {
    std::ofstream f(blacklistPath(), std::ios::trunc);
    for (int id : g_blacklist) f << id << "\n";
}

void writeLastLevel(int id) {
    std::ofstream f(lastCrashPath(), std::ios::trunc);
    f << id;
}

void clearLastLevel() {
    std::ofstream f(lastCrashPath(), std::ios::trunc);
    f << 0;
}

void flushGameSave() {
    if (auto gm = GameManager::sharedState()) {
        try {
            gm->save();
            log::info("[AutoGrinder] GameManager::save() OK");
        } catch (...) {
            log::warn("[AutoGrinder] GameManager::save() ошибка");
        }
    }
}

// ==================== NICE SHOT! (Куб 50) ====================
void unlockNiceShot() {
    const char* id = "geometry.ach.secret12"; // Nice shot! / Куб 50

    auto am = AchievementManager::sharedState();
    if (!am) {
        log::error("[AutoGrinder] AchievementManager null");
        return;
    }

    // 1) Прогресс 100% в reportedAchievements
    if (am->m_reportedAchievements) {
        am->m_reportedAchievements->setObject(
            cocos2d::CCString::create("100"),
            id
        );
        log::info("[AutoGrinder] reportedAchievements = 100");
    }

    // 2) Отметить как полученное
    if (am->m_achievementUnlocks) {
        am->m_achievementUnlocks->setObject(
            cocos2d::CCString::create("1"),
            id
        );
        log::info("[AutoGrinder] achievementUnlocks = 1");
    }

    // 3) Сохранить unlock'и
    try {
        am->storeAchievementUnlocks();
        log::info("[AutoGrinder] storeAchievementUnlocks OK");
    } catch (...) {
        log::warn("[AutoGrinder] storeAchievementUnlocks fail");
    }

    // 4) Баннер в игре
    try {
        am->notifyAchievementWithID(id);
        log::info("[AutoGrinder] notifyAchievementWithID OK");
    } catch (...) {
        log::warn("[AutoGrinder] notifyAchievementWithID fail");
    }

    try {
        am->checkAchFromUnlock(id);
    } catch (...) {}

    // 5) Иконка (на всякий случай)
    if (auto gm = GameManager::sharedState()) {
        try { gm->unlockIcon(49, IconType::Cube); } catch (...) {}
        try { gm->unlockIcon(50, IconType::Cube); } catch (...) {}
        try { gm->save(); } catch (...) {}
    }

    // Проверка
    bool earned = false;
    try {
        earned = am->isAchievementEarned(id);
    } catch (...) {}
    log::info("[AutoGrinder] Nice shot! isAchievementEarned = {}", earned);
}

// ==================== SPEED ====================
void applySpeed(float s) {
    s = std::clamp(s, 1.0f, MAX_CLASSIC_SPEED);
    if (auto sch = CCDirector::get()->getScheduler())
        sch->setTimeScale(s);
}

void resetSpeed() {
    if (auto sch = CCDirector::get()->getScheduler())
        sch->setTimeScale(1.0f);
    g_currentSpeed = 1.0f;
}

void resetLevelTimers() {
    g_levelPlayTime = 0.0f;
    g_at100Timer = 0.0f;
    g_noMoveTimer = 0.0f;
    g_lastPlayerX = -99999.f;
    g_completing = false;
    g_inputFrame = 0;
    g_watchdogSilent = 0;
}

// ==================== ЗАЧЁТ ПРОХОЖДЕНИЯ ====================
void markLevelFullyCompleted(GJGameLevel* level) {
    if (!level) return;

    level->m_normalPercent = 100;
    if (level->m_practicePercent < 100)
        level->m_practicePercent = 100;

    if (auto gm = GameManager::sharedState()) {
        try {
            gm->reportPercentageForLevel(level->m_levelID, 100, false);
            log::info("[AutoGrinder] reportPercentageForLevel(100) id={}", (int)level->m_levelID);
        } catch (...) {
            log::warn("[AutoGrinder] reportPercentageForLevel fail");
        }
    }

    if (auto gsm = GameStatsManager::sharedState()) {
        try {
            gsm->completedLevel(level);
            log::info("[AutoGrinder] completedLevel OK");
        } catch (...) {}
        try {
            gsm->awardCurrencyForLevel(level);
            log::info("[AutoGrinder] awardCurrencyForLevel OK");
        } catch (...) {}
    }
}

void safeComplete(PlayLayer* pl) {
    if (!pl || g_completing) return;

    g_completing = true;
    g_state = BotState::Waiting;
    g_waitTimer = AFTER_COMPLETE_WAIT;
    resetSpeed();

    markLevelFullyCompleted(pl->m_level);

    clearLastLevel();
    g_sessionCompleted++;
    log::info("[AutoGrinder] COMPLETE + report | session={} | id={}",
        g_sessionCompleted,
        pl->m_level ? (int)pl->m_level->m_levelID : 0);

    try {
        pl->levelComplete();
    } catch (...) {
        log::error("[AutoGrinder] levelComplete exception → quit");
        try { pl->onQuit(); } catch (...) {}
        return;
    }

    if (g_sessionCompleted % 15 == 0)
        flushGameSave();
}

void safeQuit(PlayLayer* pl) {
    if (!pl) return;
    g_completing = true;
    g_state = BotState::Waiting;
    g_waitTimer = BETWEEN_LEVELS_WAIT;
    resetSpeed();
    log::info("[AutoGrinder] safeQuit()");
    try { pl->onQuit(); }
    catch (...) { log::error("[AutoGrinder] onQuit exception"); }
}

// ==================== СБОР УРОВНЕЙ ====================
void collectFromList() {
    g_levelQueue.clear();
    g_currentIndex = 0;
    if (!g_currentList) return;

    auto glm = GameLevelManager::sharedState();
    if (!glm) return;

    for (int id : g_currentList->m_levels) {
        if (id <= 20) continue;
        if (g_blacklist.count(id)) continue;

        auto full = glm->getSavedLevel(id);
        if (full) {
            g_levelQueue.push_back(full);
        } else {
            auto stub = GJGameLevel::create();
            if (stub) {
                stub->m_levelID = id;
                g_levelQueue.push_back(stub);
            }
        }
    }
    log::info("[AutoGrinder] LIST: {} уровней", g_levelQueue.size());
}

void collectLevelsFromBrowser() {
    g_levelQueue.clear();
    g_currentIndex = 0;

    if (g_listMode) {
        collectFromList();
        if (!g_levelQueue.empty()) return;
    }

    if (!g_browser) {
        log::warn("[AutoGrinder] Браузер не найден");
        return;
    }

    if (g_browser->m_searchObject)
        g_currentSearch = g_browser->m_searchObject;

    auto glm = GameLevelManager::sharedState();
    if (!glm) return;

    if (g_browser->m_searchObject) {
        auto key = g_browser->m_searchObject->getKey();
        if (auto results = glm->getStoredOnlineLevels(key)) {
            for (auto lvl : CCArrayExt<GJGameLevel*>(results)) {
                if (!lvl || lvl->m_levelID <= 20) continue;
                if (g_blacklist.count(lvl->m_levelID)) continue;
                g_levelQueue.push_back(lvl);
            }
        }
    }

    if (g_levelQueue.empty() && g_browser->m_list && g_browser->m_list->m_listView) {
        if (auto entries = g_browser->m_list->m_listView->m_entries) {
            for (auto node : CCArrayExt<CCNode*>(entries)) {
                if (auto cell = typeinfo_cast<LevelCell*>(node)) {
                    if (cell->m_level && cell->m_level->m_levelID > 20
                        && !g_blacklist.count(cell->m_level->m_levelID))
                        g_levelQueue.push_back(cell->m_level);
                }
            }
        }
    }

    log::info("[AutoGrinder] Собрано: {}", g_levelQueue.size());
}

void startNextLevel() {
    if (g_currentIndex >= g_levelQueue.size()) {
        if (g_listMode) {
            log::info("[AutoGrinder] ===== LIST ЗАКОНЧЕН ===== | +{}", g_sessionCompleted);
            flushGameSave();
            g_state = BotState::Idle;
            resetSpeed();
            return;
        }

        log::info("[AutoGrinder] ===== СТРАНИЦА ЗАКОНЧЕНА =====");
        if (g_currentSearch) {
            int nextPage = g_currentSearch->m_page + 1;
            auto nextSearch = g_currentSearch->getPageObject(nextPage);
            if (nextSearch) {
                log::info("[AutoGrinder] Страница {}", nextPage);
                g_currentSearch = nextSearch;
                g_levelQueue.clear();
                g_currentIndex = 0;
                resetSpeed();
                resetLevelTimers();
                if (auto scene = LevelBrowserLayer::scene(nextSearch))
                    CCDirector::get()->replaceScene(CCTransitionFade::create(0.35f, scene));
                g_state = BotState::Collecting;
                g_waitTimer = 3.2f;
                return;
            }
        }
        log::warn("[AutoGrinder] Конец, стоп");
        flushGameSave();
        g_state = BotState::Idle;
        resetSpeed();
        return;
    }

    auto level = g_levelQueue[g_currentIndex];
    if (!level || level->m_levelID <= 20) {
        g_currentIndex++;
        g_waitTimer = 0.4f;
        g_state = BotState::Waiting;
        return;
    }

    int id = level->m_levelID;
    if (g_blacklist.count(id)) {
        g_currentIndex++;
        g_waitTimer = 0.3f;
        g_state = BotState::Waiting;
        return;
    }

    auto glm = GameLevelManager::sharedState();
    if (!glm) {
        g_currentIndex++;
        g_waitTimer = 0.5f;
        g_state = BotState::Waiting;
        return;
    }

    auto full = glm->getSavedLevel(id);
    if (!full || full->m_levelString.size() < 20) {
        if (g_lastDownloadID != id) {
            g_lastDownloadID = id;
            g_downloadRetries = 0;
        }
        if (g_downloadRetries == 0) {
            log::info("[AutoGrinder] Скачиваю {}", id);
            try { glm->downloadLevel(id, false, 0); } catch (...) {}
        }
        g_downloadRetries++;
        if (g_downloadRetries > 8) {
            log::warn("[AutoGrinder] Не скачался {} → blacklist", id);
            g_blacklist.insert(id);
            saveBlacklist();
            g_currentIndex++;
            g_lastDownloadID = 0;
            g_downloadRetries = 0;
        }
        g_waitTimer = 2.4f;
        g_state = BotState::Waiting;
        return;
    }

    g_lastDownloadID = 0;
    g_downloadRetries = 0;
    g_lastPlayedID = id;
    writeLastLevel(id);

    log::info("[AutoGrinder] Играю: {} ({}) [{}/{}]{}",
        full->m_levelName.c_str(), id,
        g_currentIndex + 1, g_levelQueue.size(),
        g_listMode ? " [LIST]" : "");

    cocos2d::CCScene* scene = nullptr;
    try { scene = PlayLayer::scene(full, false, false); }
    catch (...) { scene = nullptr; }

    if (!scene) {
        log::warn("[AutoGrinder] Сцена fail {} → blacklist", id);
        g_blacklist.insert(id);
        saveBlacklist();
        g_currentIndex++;
        g_waitTimer = 0.8f;
        g_state = BotState::Waiting;
        return;
    }

    resetLevelTimers();
    g_state = BotState::Playing;
    CCDirector::get()->replaceScene(CCTransitionFade::create(0.15f, scene));
}

// ==================== SETTINGS ====================
$on_mod(Loaded) {
    loadBlacklist();

    g_botEnabled = Mod::get()->getSettingValue<bool>("bot-enabled");
    g_baseSpeed = (float)Mod::get()->getSettingValue<double>("base-speed");
    g_minFPS = (int)Mod::get()->getSettingValue<int64_t>("min-fps-threshold");
    g_noclipEnabled = Mod::get()->getSettingValue<bool>("enable-noclip");
    g_autoclickerEnabled = Mod::get()->getSettingValue<bool>("enable-autoclicker");
    g_autoclickerMinFPS = (int)Mod::get()->getSettingValue<int64_t>("autoclicker-min-fps");
    g_currentSpeed = std::clamp(g_baseSpeed, 1.0f, MAX_CLASSIC_SPEED);

    listenForSettingChanges<bool>("bot-enabled", [](bool v) {
        g_botEnabled = v;
        if (v) {
            g_currentSpeed = std::clamp(g_baseSpeed, 1.0f, MAX_CLASSIC_SPEED);
            g_state = BotState::Collecting;
            g_waitTimer = 1.0f;
            resetLevelTimers();
            g_sessionCompleted = 0;
            log::info("[AutoGrinder] БОТ ВКЛ | listMode={}", g_listMode);
        } else {
            resetSpeed();
            flushGameSave();
            g_state = BotState::Idle;
            g_levelQueue.clear();
            resetLevelTimers();
            log::info("[AutoGrinder] БОТ ВЫКЛ | за сессию: {}", g_sessionCompleted);
        }
    });

    listenForSettingChanges<double>("base-speed", [](double v) {
        g_baseSpeed = (float)v;
        if (g_botEnabled && g_state == BotState::Playing && !g_isPlatformer) {
            g_currentSpeed = std::clamp(g_baseSpeed, 1.0f, MAX_CLASSIC_SPEED);
            applySpeed(g_currentSpeed);
        }
    });

    listenForSettingChanges<int64_t>("min-fps-threshold", [](int64_t v) { g_minFPS = (int)v; });
    listenForSettingChanges<bool>("enable-noclip", [](bool v) { g_noclipEnabled = v; });
    listenForSettingChanges<bool>("enable-autoclicker", [](bool v) { g_autoclickerEnabled = v; });
    listenForSettingChanges<int64_t>("autoclicker-min-fps", [](int64_t v) { g_autoclickerMinFPS = (int)v; });

    listenForKeybindSettingPresses("toggle-bot-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat)
            Mod::get()->setSettingValue("bot-enabled", !Mod::get()->getSettingValue<bool>("bot-enabled"));
    });

    listenForKeybindSettingPresses("open-settings-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) openSettingsPopup(Mod::get());
    });

    // Nice shot! (Куб 50) — игра + Steam
    listenForKeybindSettingPresses("unlock-nice-shot-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat)
            unlockNiceShot();
    });
}

// ==================== LIST ====================
class $modify(MyLevelListLayer, LevelListLayer) {
    bool init(GJLevelList* list) {
        if (!LevelListLayer::init(list)) return false;
        g_browser = this;
        g_listMode = true;
        g_currentList = list;
        g_currentSearch = nullptr;
        log::info("[AutoGrinder] LIST: {} ({})",
            list ? list->m_listName.c_str() : "?",
            list ? list->m_listID : 0);
        if (g_botEnabled) {
            g_state = BotState::Collecting;
            g_waitTimer = 2.5f;
        }
        return true;
    }

    void loadLevelsFinished(CCArray* levels, char const* key, int type) {
        LevelListLayer::loadLevelsFinished(levels, key, type);
        if (g_botEnabled && g_listMode && g_state == BotState::Idle) {
            g_state = BotState::Collecting;
            g_waitTimer = 1.5f;
        }
    }

    void onExit() {
        if (g_browser == this) g_browser = nullptr;
        g_listMode = false;
        g_currentList = nullptr;
        LevelListLayer::onExit();
    }
};

// ==================== BROWSER ====================
class $modify(MyLevelBrowserLayer, LevelBrowserLayer) {
    bool init(GJSearchObject* s) {
        if (!LevelBrowserLayer::init(s)) return false;
        if (!typeinfo_cast<LevelListLayer*>(this)) {
            g_browser = this;
            g_listMode = false;
            g_currentList = nullptr;
            if (s) g_currentSearch = s;
        }
        return true;
    }

    void loadPage(GJSearchObject* s) {
        LevelBrowserLayer::loadPage(s);
        if (g_botEnabled && !g_listMode && g_state == BotState::Idle) {
            g_state = BotState::Collecting;
            g_waitTimer = 2.2f;
        }
    }

    void onExit() {
        if (g_browser == this && !g_listMode) g_browser = nullptr;
        LevelBrowserLayer::onExit();
    }
};

// ==================== PLAYLAYER ====================
class $modify(MyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool a, bool b) {
        if (!PlayLayer::init(level, a, b)) {
            if (g_botEnabled) {
                if (level && level->m_levelID > 20) {
                    g_blacklist.insert(level->m_levelID);
                    saveBlacklist();
                }
                g_currentIndex++;
                g_state = BotState::Waiting;
                g_waitTimer = 1.0f;
            }
            return false;
        }

        if (g_botEnabled) {
            g_state = BotState::Playing;
            resetLevelTimers();

            g_isPlatformer = false;
            if (level) g_isPlatformer = level->isPlatformer();
            if (!g_isPlatformer && m_player1)
                g_isPlatformer = m_player1->m_isPlatformer;

            if (g_isPlatformer) {
                applySpeed(PLATFORMER_SPEED);
                g_currentSpeed = PLATFORMER_SPEED;
                log::info("[AutoGrinder] ПЛАТФОРМЕР {} | 1.0x", level ? (int)level->m_levelID : 0);
            } else {
                g_currentSpeed = std::clamp(g_baseSpeed, 1.0f, MAX_CLASSIC_SPEED);
                applySpeed(g_currentSpeed);
                log::info("[AutoGrinder] Классика {} | {:.1f}x", level ? (int)level->m_levelID : 0, g_currentSpeed);
            }
        }
        return true;
    }

    void levelComplete() {
        markLevelFullyCompleted(m_level);
        PlayLayer::levelComplete();
        if (g_botEnabled) {
            log::info("[AutoGrinder] levelComplete (native/hook)");
            g_completing = true;
            g_state = BotState::Waiting;
            g_waitTimer = AFTER_COMPLETE_WAIT;
            resetSpeed();
            g_levelPlayTime = 0.0f;
            clearLastLevel();
        }
    }

    void onQuit() {
        resetSpeed();
        PlayLayer::onQuit();
        if (g_botEnabled) {
            g_currentIndex++;
            g_state = BotState::Waiting;
            g_waitTimer = BETWEEN_LEVELS_WAIT;
            g_isPlatformer = false;
            resetLevelTimers();
        }
    }

    void update(float dt) {
        PlayLayer::update(dt);
        if (!g_botEnabled || g_state != BotState::Playing || g_completing) return;
        if (g_isPlatformer) return;

        static float fps = 60.f;
        static float lastLog = -1.f;
        float inst = dt > 0.0001f ? 1.f / dt : 60.f;
        fps = fps * 0.7f + inst * 0.3f;

        if (fps < g_minFPS) {
            float factor = fps / (float)std::max(1, g_minFPS);
            float target = std::max(1.2f, g_baseSpeed * factor * 0.75f);
            g_currentSpeed = g_currentSpeed * 0.55f + target * 0.45f;
            applySpeed(g_currentSpeed);
            if (std::abs(g_currentSpeed - lastLog) > 0.6f) {
                log::info("[AutoGrinder] FPS: {:.0f} → {:.1f}x", fps, g_currentSpeed);
                lastLog = g_currentSpeed;
            }
        } else if (fps > g_minFPS + 20.f && g_currentSpeed < g_baseSpeed) {
            g_currentSpeed = std::min(g_baseSpeed, g_currentSpeed * 1.025f);
            applySpeed(g_currentSpeed);
        }

        if (g_autoclickerEnabled && fps >= g_autoclickerMinFPS)
            this->handleButton(true, 1, true);

        float percent = 0.f;
        if (m_player1 && m_levelLength > 10.f) {
            percent = getCurrentPercent();
            float p2 = (m_player1->getPositionX() / m_levelLength) * 100.f;
            if (p2 > percent) percent = p2;
        }

        if (m_player1) {
            float x = m_player1->getPositionX();
            if (x > 40000.f || x < -3000.f) {
                if (g_lastPlayedID > 20) {
                    g_blacklist.insert(g_lastPlayedID);
                    saveBlacklist();
                }
                safeQuit(this);
                return;
            }
            if (std::abs(x - g_lastPlayerX) < 5.0f) g_noMoveTimer += dt;
            else { g_noMoveTimer = 0.0f; g_lastPlayerX = x; }
        }

        if (percent >= 99.0f) g_at100Timer += dt;
        else g_at100Timer = 0.0f;

        g_levelPlayTime += dt;

        if (g_at100Timer > STUCK_100_TIME) {
            log::warn("[AutoGrinder] 100% stuck → safeComplete");
            safeComplete(this);
            return;
        }
        if (percent > 75.f && g_noMoveTimer > NO_MOVE_TIME) {
            log::warn("[AutoGrinder] no move → safeComplete");
            safeComplete(this);
            return;
        }
        if (g_levelPlayTime > MAX_TIME_CLASSIC) {
            log::warn("[AutoGrinder] classic timeout → safeComplete");
            safeComplete(this);
            return;
        }
    }
};

// ==================== NOCLIP ====================
class $modify(MyPlayerObject, PlayerObject) {
    void collidedWithObject(float a, GameObject* obj, cocos2d::CCRect r, bool b) {
        if (g_botEnabled && g_noclipEnabled && !g_isPlatformer
            && g_state == BotState::Playing && !g_completing)
            return;
        PlayerObject::collidedWithObject(a, obj, r, b);
    }
};

// ==================== SCHEDULER ====================
class $modify(MyScheduler, CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);
        if (!g_botEnabled) return;

        if (g_state == BotState::Playing) {
            auto pl = PlayLayer::get();
            if (!pl) {
                g_state = BotState::Waiting;
                g_waitTimer = 1.2f;
                resetSpeed();
                return;
            }
            if (g_completing) return;

            if (g_isPlatformer) {
                if (g_currentSpeed > 1.05f) {
                    applySpeed(PLATFORMER_SPEED);
                    g_currentSpeed = PLATFORMER_SPEED;
                }

                float prev = g_levelPlayTime;
                g_levelPlayTime += dt;
                if (g_levelPlayTime - prev < 0.0001f) g_watchdogSilent++;
                else g_watchdogSilent = 0;

                if (g_watchdogSilent > 120) {
                    safeQuit(pl);
                    return;
                }

                static int lastSec = -1;
                int sec = (int)g_levelPlayTime;
                if (sec != lastSec && sec <= 12) {
                    lastSec = sec;
                    float x = pl->m_player1 ? pl->m_player1->getPositionX() : 0.f;
                    log::info("[AutoGrinder] Платформер {}с | x={:.0f}", sec, x);
                }

                g_inputFrame++;
                if (g_autoclickerEnabled && (g_inputFrame % 4 == 0)) {
                    pl->handleButton(true, 1, true);
                    pl->handleButton(true, 3, true);
                }

                if (g_levelPlayTime > FORCE_COMPLETE_PLAT) {
                    log::info("[AutoGrinder] FORCE COMPLETE платформер ({}с)", FORCE_COMPLETE_PLAT);
                    safeComplete(pl);
                    return;
                }
                if (g_levelPlayTime > MAX_TIME_PLATFORMER) {
                    safeQuit(pl);
                    return;
                }
            }
            return;
        }

        if (g_state == BotState::Collecting || g_state == BotState::Waiting) {
            g_waitTimer -= dt;
            if (g_waitTimer > 0.0f) return;

            if (g_state == BotState::Collecting) {
                collectLevelsFromBrowser();
                if (!g_levelQueue.empty()) {
                    g_state = BotState::Waiting;
                    g_waitTimer = 0.8f;
                } else {
                    g_waitTimer = 4.0f;
                    log::info("[AutoGrinder] 0 уровней, жду...");
                }
                return;
            }

            if (auto pl = PlayLayer::get()) {
                safeQuit(pl);
                return;
            }
            startNextLevel();
        }
    }
};