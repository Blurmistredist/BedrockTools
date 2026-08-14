#include "videosettings.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include "core/memory/Hooks.hpp"

#include <cstdint>

namespace {

using RenderLevelFn = void (*)(void*, void*, void*);

RenderLevelFn g_originalRenderLevel = nullptr;
VideoSettingsModule* g_videoSettings = nullptr;

void renderLevelHook(void* self, void* screenContext, void* a3) {
    auto* module = g_videoSettings;

    // IMPORTANT:
    // We deliberately skip only the LevelRenderer world pass. We do not
    // touch the HUD/UI renderer, ModMenu, input, or FPS Graph.
    if (module && module->m_worldRenderingTest) {
        return;
    }

    if (g_originalRenderLevel) {
        g_originalRenderLevel(self, screenContext, a3);
    }
}

} // namespace

VideoSettingsModule::VideoSettingsModule()
    : Module(
          "Video Settings",
          "In-game rendering controls and a world-rendering FPS benchmark.") {
    showInMenu = true;
    g_videoSettings = this;
}

VideoSettingsModule::~VideoSettingsModule() {
    if (g_videoSettings == this) {
        g_videoSettings = nullptr;
    }
}

void VideoSettingsModule::onInit() {
    if (m_patchTarget) return;

    const std::uintptr_t address =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderLevel);

    if (address != 0) {
        m_patchTarget = reinterpret_cast<void*>(address);
    }
}

void VideoSettingsModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;

    if (bedrocktools::hooks::install(
            m_patchTarget,
            reinterpret_cast<void*>(renderLevelHook),
            reinterpret_cast<void**>(&g_originalRenderLevel))) {
        m_patched = true;
    }
}

void VideoSettingsModule::onEnable() {
    applyPatch();
}

void VideoSettingsModule::onDisable() {
    // The hook stays installed for the lifetime of the process. Disabling
    // the module simply restores normal behavior by forwarding the call.
}

void VideoSettingsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    m_worldRenderingTest = j.value("m_worldRenderingTest", m_worldRenderingTest);
    m_showPanel = j.value("m_showPanel", m_showPanel);
    hudPosX = j.value("hudPosX", hudPosX);
    hudPosY = j.value("hudPosY", hudPosY);
}

void VideoSettingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_worldRenderingTest"] = m_worldRenderingTest;
    j["m_showPanel"] = m_showPanel;
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
}

void VideoSettingsModule::onFrame() {
    if (!enabled || !m_showPanel) return;

    std::vector<PLModMenu_DrawCommand> cmds;
    std::vector<std::string> strings;

    auto text = [&](float px, float py, float size, std::uint32_t color, const std::string& s) {
        strings.push_back(s);
        PLModMenu_DrawCommand c{};
        c.type = PL_DRAW_TEXT;
        c.x = px; c.y = py; c.w = 0.f; c.h = 1.f;
        c.size = size; c.color = color; c.text = strings.back().c_str();
        cmds.push_back(c);
    };
    auto rect = [&](float px, float py, float pw, float ph, std::uint32_t color) {
        PLModMenu_DrawCommand c{};
        c.type = PL_DRAW_RECT_FILLED;
        c.x = px; c.y = py; c.w = pw; c.h = ph; c.color = color;
        cmds.push_back(c);
    };

    const float w = 430.f;
    const float h = 150.f;
    rect(hudPosX, hudPosY, w, h, 0xEE10161Cu);
    rect(hudPosX + 3.f, hudPosY + 3.f, w - 6.f, h - 6.f, 0xE9161D25u);
    text(hudPosX + 18.f, hudPosY + 14.f, 24.f, 0xFFE5EDF7u, "VIDEO SETTINGS");
    text(hudPosX + 18.f, hudPosY + 52.f, 18.f, 0xFFB5C3D1u, "World Rendering Test");
    text(hudPosX + 330.f, hudPosY + 52.f, 18.f,
         m_worldRenderingTest ? 0xFF2BEA76u : 0xFF8897A7u,
         m_worldRenderingTest ? "ON" : "OFF");
    text(hudPosX + 18.f, hudPosY + 85.f, 16.f, 0xFF8FA0B1u,
         "GUI / HUD remains active while the test is enabled.");
    text(hudPosX + 18.f, hudPosY + 112.f, 15.f, 0xFF718394u,
         "Use the module menu to toggle the test.");

    submitDrawCommands(moduleId, cmds);
}
