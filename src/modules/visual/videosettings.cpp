#include "videosettings.hpp"

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

    m_worldRenderingTest =
        j.value("m_worldRenderingTest", m_worldRenderingTest);
}

void VideoSettingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_worldRenderingTest"] = m_worldRenderingTest;
}
