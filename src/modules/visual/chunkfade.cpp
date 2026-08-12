#include "chunkfade.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
ChunkFadeModule* g_chunkFade = nullptr;

void (*g_setupFogOriginal)(void*, void*, float) = nullptr;

int (*g_getPerspectiveOriginal)(void*) = nullptr;
bool g_perspectiveHooked = false;
bool g_thirdPerson = false;

void setupFogHook(void* self, void* screenContext, float a3) {
    if (g_setupFogOriginal)
        g_setupFogOriginal(self, screenContext, a3);

    if (!g_chunkFade || !g_chunkFade->enabled || !self)
        return;

    if (g_chunkFade->onlyThirdPerson() && !g_thirdPerson)
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(self);
    float& fogStart = *reinterpret_cast<float*>(
        base + bedrocktools::sdk::offsets::LevelRendererPlayer::mBaseFogStart);
    float& fogEnd = *reinterpret_cast<float*>(
        base + bedrocktools::sdk::offsets::LevelRendererPlayer::mBaseFogEnd);

    // Start from Minecraft's calculated values and offset them. This keeps
    // the effect as normal world fog rather than replacing the sky renderer.
    const float originalStart = fogStart;
    const float originalEnd = fogEnd;

    float start = originalStart + g_chunkFade->fadeStart();
    float end = originalEnd + g_chunkFade->fadeEnd();

    // Never allow an invalid/inverted fog interval.
    end = std::max(end, start + 0.25f);

    fogStart = start;
    fogEnd = end;

    // Opacity is intentionally applied only to the fog density value.
    // We do not touch fog RGB, so the skybox retains its normal color.
    float& density = *reinterpret_cast<float*>(
        base + bedrocktools::sdk::offsets::LevelRendererPlayer::mCurrentFogDensityMax);

    const float opacity = std::clamp(g_chunkFade->fadeOpacity(), 0.0f, 1.0f);
    density = std::max(0.0f, density * opacity);
}

int perspectiveHook(void* self) {
    const int result = g_getPerspectiveOriginal ? g_getPerspectiveOriginal(self) : 0;
    g_thirdPerson = (result != 0);
    return result;
}
}

ChunkFadeModule::ChunkFadeModule()
    : Module("Chunk Fade", "Adjusts normal Minecraft fog distance without changing the skybox.") {
    g_chunkFade = this;
}

ChunkFadeModule::~ChunkFadeModule() {
    if (g_chunkFade == this) g_chunkFade = nullptr;
}

void ChunkFadeModule::onInit() {
    if (!m_hooked) {
        const auto addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::SetupFogPlayer);
        if (addr) {
            m_patchTarget = reinterpret_cast<void*>(addr);
            m_hooked = static_cast<bool>(bedrocktools::hooks::install(
                m_patchTarget,
                reinterpret_cast<void*>(setupFogHook),
                reinterpret_cast<void**>(&g_setupFogOriginal)));
        }
    }

    if (!g_perspectiveHooked) {
        const auto addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::GetPerspective);
        if (addr) {
            g_perspectiveHooked = static_cast<bool>(bedrocktools::hooks::install(
                reinterpret_cast<void*>(addr),
                reinterpret_cast<void*>(perspectiveHook),
                reinterpret_cast<void**>(&g_getPerspectiveOriginal)));
        }
    }
}

void ChunkFadeModule::onEnable() {}
void ChunkFadeModule::onDisable() {}

void ChunkFadeModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_fadeStart = j.value("m_fadeStart", m_fadeStart);
    m_fadeEnd = j.value("m_fadeEnd", m_fadeEnd);
    m_fadeOpacity = j.value("m_fadeOpacity", m_fadeOpacity);
    m_onlyThirdPerson = j.value("m_onlyThirdPerson", m_onlyThirdPerson);
}

void ChunkFadeModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_fadeStart"] = m_fadeStart;
    j["m_fadeEnd"] = m_fadeEnd;
    j["m_fadeOpacity"] = m_fadeOpacity;
    j["m_onlyThirdPerson"] = m_onlyThirdPerson;
}
