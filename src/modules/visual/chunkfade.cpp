#include "chunkfade.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/ClientInstance.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <cmath>

static ChunkFadeModule* g_chunkFadeMod = nullptr;
static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3) = nullptr;
static int (*_getPerspective_orig)(void* _this) = nullptr;

bool ChunkFadeModule::isThirdPerson() const {
    return m_thirdPerson;
}

static int _getPerspective_hook(void* _this) {
    int result = 0;
    if (_getPerspective_orig) {
        result = _getPerspective_orig(_this);
    }
    if (g_chunkFadeMod) {
        g_chunkFadeMod->m_thirdPerson = (result != 0);
    }
    return result;
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    struct FogState {
        float* start = nullptr;
        float* end = nullptr;
        float* density = nullptr;
        float savedStart = 0.0f;
        float savedEnd = 0.0f;
        float savedDensity = 0.0f;
        bool valid = false;
    } fog;

    if (g_chunkFadeMod && g_chunkFadeMod->enabled) {
        auto* levelRenderer = reinterpret_cast<bedrocktools::sdk::LevelRenderer*>(_this);
        auto* lrp = levelRenderer ? levelRenderer->playerRenderer() : nullptr;
        if (lrp) {
            fog.start = &lrp->baseFogStart();
            fog.end = &lrp->baseFogEnd();
            fog.density = &lrp->currentFogDensityMax();
            fog.savedStart = *fog.start;
            fog.savedEnd = *fog.end;
            fog.savedDensity = *fog.density;
            fog.valid = true;

            const float fadeStart = std::max(0.0f, g_chunkFadeMod->m_fadeStart);
            const float fadeEnd = std::max(fadeStart + 1.0f, g_chunkFadeMod->m_fadeEnd);
            const float fadeDensity = std::clamp(g_chunkFadeMod->m_fadeOpacity, 0.0f, 1.0f);

            const bool shouldApply = !g_chunkFadeMod->m_onlyThirdPerson || g_chunkFadeMod->m_thirdPerson;
            if (shouldApply) {
                *fog.start = fadeStart;
                *fog.end = fadeEnd;
                *fog.density = fadeDensity;
                lrp->fogColorRed() = std::clamp(g_chunkFadeMod->m_fadeColorR, 0.0f, 1.0f);
                lrp->fogColorGreen() = std::clamp(g_chunkFadeMod->m_fadeColorG, 0.0f, 1.0f);
                lrp->fogColorBlue() = std::clamp(g_chunkFadeMod->m_fadeColorB, 0.0f, 1.0f);
            }
        }
    }

    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (fog.valid) {
        *fog.start = fog.savedStart;
        *fog.end = fog.savedEnd;
        *fog.density = fog.savedDensity;
    }
}

ChunkFadeModule::ChunkFadeModule()
    : Module("Chunk Fade", "Smooths distant chunk transitions with a configurable fog fade.") {
    g_chunkFadeMod = this;
}

ChunkFadeModule::~ChunkFadeModule() {
    if (g_chunkFadeMod == this) g_chunkFadeMod = nullptr;
}

void ChunkFadeModule::onInit() {
    if (!m_perspectiveTarget) {
        const uintptr_t p = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetPerspective);
        if (p != 0) m_perspectiveTarget = reinterpret_cast<void*>(p);
    }
    if (!m_patchTarget) {
        const uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
        if (addr != 0) m_patchTarget = reinterpret_cast<void*>(addr);
    }

    if (m_perspectiveTarget && !_getPerspective_orig) {
        bedrocktools::hooks::install(m_perspectiveTarget, reinterpret_cast<void*>(_getPerspective_hook), reinterpret_cast<void**>(&_getPerspective_orig));
    }
}

void ChunkFadeModule::onEnable() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(_renderLevel_hook), reinterpret_cast<void**>(&_renderLevel_orig));
    m_patched = true;
}

void ChunkFadeModule::onDisable() {
}

void ChunkFadeModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_fadeStart")) m_fadeStart = j["m_fadeStart"].get<float>();
    if (j.contains("m_fadeEnd")) m_fadeEnd = j["m_fadeEnd"].get<float>();
    if (j.contains("m_fadeOpacity")) m_fadeOpacity = j["m_fadeOpacity"].get<float>();
    if (j.contains("m_fadeColorR")) m_fadeColorR = j["m_fadeColorR"].get<float>();
    if (j.contains("m_fadeColorG")) m_fadeColorG = j["m_fadeColorG"].get<float>();
    if (j.contains("m_fadeColorB")) m_fadeColorB = j["m_fadeColorB"].get<float>();
    if (j.contains("m_onlyThirdPerson")) m_onlyThirdPerson = j["m_onlyThirdPerson"].get<bool>();
}

void ChunkFadeModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_fadeStart"] = m_fadeStart;
    j["m_fadeEnd"] = m_fadeEnd;
    j["m_fadeOpacity"] = m_fadeOpacity;
    j["m_fadeColorR"] = m_fadeColorR;
    j["m_fadeColorG"] = m_fadeColorG;
    j["m_fadeColorB"] = m_fadeColorB;
    j["m_onlyThirdPerson"] = m_onlyThirdPerson;
}
