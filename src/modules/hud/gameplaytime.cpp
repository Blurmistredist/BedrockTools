#include "gameplaytime.hpp"
#include "../ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/Events.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
GameplayTimeModule* g_gameplayTime = nullptr;

using ClientInstanceGetLocalPlayer_t = void* (*)(void*);

ClientInstanceGetLocalPlayer_t g_getLocalPlayer = nullptr;

static float textWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ')
            width += size * 0.30f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
            width += size * 0.80f;
        else
            width += size * 0.58f;
    }
    return width;
}

static void gameplayTimeTickCallback(void* player) {
    if (g_gameplayTime)
        g_gameplayTime->onLocalPlayerTick(player);
}

static void gameplayTimeClientCallback(void* clientInstance) {
    if (g_gameplayTime)
        g_gameplayTime->onClientUpdate(clientInstance);
}
}

GameplayTimeModule::GameplayTimeModule()
    : Module(
        "Gameplay Time",
        "Shows how long the current gameplay session has been active."
      ) {
    g_gameplayTime = this;
}

void GameplayTimeModule::onInit() {
    g_getLocalPlayer =
        reinterpret_cast<ClientInstanceGetLocalPlayer_t>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ClientInstanceGetLocalPlayer
            )
        );

    bedrocktools::events::bus().subscribe<
        bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            gameplayTimeTickCallback(event.player);
        }
    );

    bedrocktools::events::bus().subscribe<
        bedrocktools::events::ClientInstanceUpdateEvent>(
        [](auto& event) {
            gameplayTimeClientCallback(event.clientInstance);
        }
    );
}

void GameplayTimeModule::onEnable() {
    /*
     * Enabling the module while already in a world starts a fresh session
     * from the first LocalPlayerTick. Disabling it does not alter the
     * underlying session state.
     */
    m_missingPlayerUpdates = 0;
}

void GameplayTimeModule::onDisable() {
    // Keep session state intact. Re-enabling while still in-game continues
    // the current session rather than creating a new one.
}

void GameplayTimeModule::onLocalPlayerTick(void* player) {
    if (!player)
        return;

    m_missingPlayerUpdates = 0;

    if (!m_inSession) {
        m_sessionStart = Clock::now();
        m_inSession = true;
    }
}

void GameplayTimeModule::onClientUpdate(void* clientInstance) {
    if (!clientInstance || !g_getLocalPlayer)
        return;

    /*
     * ClientInstanceUpdate is also called while menus/loading are active.
     * A null local player means the current gameplay session has ended.
     *
     * Require two consecutive missing-player updates to avoid resetting
     * during a single transient frame while the client is changing state.
     */
    void* localPlayer = g_getLocalPlayer(clientInstance);
    if (localPlayer) {
        m_missingPlayerUpdates = 0;

        if (!m_inSession) {
            m_sessionStart = Clock::now();
            m_inSession = true;
        }
        return;
    }

    if (!m_inSession)
        return;

    ++m_missingPlayerUpdates;
    if (m_missingPlayerUpdates >= 2) {
        m_inSession = false;
        m_missingPlayerUpdates = 0;
    }
}

std::chrono::steady_clock::duration GameplayTimeModule::elapsed() const {
    if (!m_inSession)
        return std::chrono::steady_clock::duration::zero();

    return Clock::now() - m_sessionStart;
}

std::string GameplayTimeModule::formatDuration(
    std::chrono::steady_clock::duration duration) const {

    const auto totalSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(duration).count();

    const long long hours = totalSeconds / 3600;
    const long long minutes = (totalSeconds % 3600) / 60;
    const long long seconds = totalSeconds % 60;

    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(2) << hours
        << ':'
        << std::setw(2) << minutes;

    if (showSeconds)
        out << ':' << std::setw(2) << seconds;

    return out.str();
}

void GameplayTimeModule::onFrame() {
    if (!enabled || !m_inSession)
        return;

    const std::string timer = formatDuration(elapsed());
    const std::string text =
        showLabel ? ("Gameplay Time: " + timer) : timer;

    const float boxW = textWidth(text, textSize) + 12.0f;
    const float boxH = textSize + 8.0f;

    std::vector<PLModMenu_DrawCommand> cmds;

    if (background) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = boxW;
        bg.h = boxH;

        const int alpha = static_cast<int>(
            std::clamp(backgroundOpacity, 0.0f, 1.0f) * 255.0f
        );
        bg.color = (static_cast<uint32_t>(alpha) << 24);
        cmds.push_back(bg);
    }

    PLModMenu_DrawCommand textCmd = {};
    textCmd.type = PL_DRAW_TEXT;
    textCmd.x = hudPosX + 6.0f;
    textCmd.y = hudPosY + 2.0f;
    textCmd.w = boxW;
    textCmd.h = boxH;
    textCmd.size = textSize;
    textCmd.color = 0xFFFFFFFF;
    textCmd.text = text.c_str();
    cmds.push_back(textCmd);

    submitDrawCommands(moduleId, cmds);
}

void GameplayTimeModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("textSize")) textSize = j["textSize"].get<float>();
    if (j.contains("background")) background = j["background"].get<bool>();
    if (j.contains("backgroundOpacity"))
        backgroundOpacity = j["backgroundOpacity"].get<float>();
    if (j.contains("showLabel")) showLabel = j["showLabel"].get<bool>();
    if (j.contains("showSeconds")) showSeconds = j["showSeconds"].get<bool>();

    textSize = std::clamp(textSize, 12.0f, 96.0f);
    backgroundOpacity = std::clamp(backgroundOpacity, 0.0f, 1.0f);
}

void GameplayTimeModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["textSize"] = textSize;
    j["background"] = background;
    j["backgroundOpacity"] = backgroundOpacity;
    j["showLabel"] = showLabel;
    j["showSeconds"] = showSeconds;
}
