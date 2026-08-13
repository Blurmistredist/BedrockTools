#include "worldclock.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {
WorldClockModule* g_worldClock = nullptr;
using TimeFn = int (*)(void*);
TimeFn g_getTime = nullptr;

void worldClockTick(bedrocktools::sdk::Player* player) {
    if (!g_worldClock || !g_worldClock->enabled || !player) return;

    auto* level = player->level();
    if (!level || !g_getTime) return;

    g_worldClock->updateWorldTicks(g_getTime(level));
}

std::string formatWorldClock(int ticks) {
    constexpr int ticksPerDay = 24000;
    int dayTicks = ticks % ticksPerDay;
    if (dayTicks < 0) dayTicks += ticksPerDay;

    // Minecraft time 0 is 06:00. Every 1000 ticks is one in-game hour.
    const int totalMinutes = 360 + (dayTicks * 1440) / ticksPerDay;
    const int hour = (totalMinutes / 60) % 24;
    const int minute = totalMinutes % 60;

    char buffer[6]{};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    return buffer;
}

float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (char c : text) {
        if (c == '1' || c == ':' ) width += size * 0.30f;
        else width += size * 0.58f;
    }
    return width;
}
}

WorldClockModule::WorldClockModule()
    : Module("World Clock", "Converts Minecraft world ticks into a 24-hour clock.") {
    g_worldClock = this;
}

WorldClockModule::~WorldClockModule() {
    if (g_worldClock == this) g_worldClock = nullptr;
}

void WorldClockModule::onInit() {
    g_getTime = reinterpret_cast<TimeFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Time)
    );

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { worldClockTick(event.player); }
    );
}

void WorldClockModule::onEnable() {
    m_worldTicks = 0;
}

void WorldClockModule::onDisable() {
    m_worldTicks = 0;
}

void WorldClockModule::onFrame() {
    if (!enabled || !g_getTime) return;

    const std::string text = formatWorldClock(m_worldTicks);
    const float boxW = calcTextWidth(text, m_size) + 4.0f;
    const float boxH = m_size + 4.0f;

    std::vector<PLModMenu_DrawCommand> cmds;

    if (m_background) {
        PLModMenu_DrawCommand bg = {};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = boxW;
        bg.h = boxH;
        const int alpha = static_cast<int>(std::clamp(m_backgroundOpacity, 0.0f, 1.0f) * 255.0f);
        bg.color = (static_cast<std::uint32_t>(alpha) << 24);
        cmds.push_back(bg);
    }

    PLModMenu_DrawCommand textCmd = {};
    textCmd.type = PL_DRAW_TEXT;
    textCmd.x = hudPosX;
    textCmd.y = hudPosY;
    textCmd.w = boxW;
    textCmd.h = boxH;
    textCmd.color = 0xFFFFFFFF;
    textCmd.size = m_size;
    textCmd.text = text.c_str();
    cmds.push_back(textCmd);

    submitDrawCommands(moduleId, cmds);
}

void WorldClockModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_size")) m_size = j["m_size"].get<float>();
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
}

void WorldClockModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_size"] = m_size;
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
}
