#include "chunkfade.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>

#include <algorithm>

static ChunkFadeModule* g_chunkFadeMod = nullptr;

static void (*_renderLevel_orig)(
    void* _this,
    void* screenContext,
    void* a3) = nullptr;

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
        g_chunkFadeMod->setThirdPersonState(result != 0);
    }

    return result;
}

static void _renderLevel_hook(
    void* _this,
    void* screenContext,
    void* a3) {

    struct FogState {
        float* start = nullptr;
        float* end = nullptr;
        float* density = nullptr;

        float savedStart = 0.0f;
        float savedEnd = 0.0f;
        float savedDensity = 0.0f;

        bool valid = false;
    } fog;

    if (g_chunkFadeMod &&
        g_chunkFadeMod->enabled) {

        auto* levelRenderer =
            reinterpret_cast<
                bedrocktools::sdk::LevelRenderer*>(_this);

        auto* lrp =
            levelRenderer
                ? levelRenderer->playerRenderer()
                : nullptr;

        if (lrp) {
            /*
             * IMPORTANT:
             *
             * Do NOT modify fogColorRed/Green/Blue here.
             *
             * The previous implementation forced the fog colour to
             * m_fadeColor*. That colour is not necessarily the same
             * colour Minecraft uses for the sky at the horizon, which
             * produced the hard horizontal line visible in-game.
             *
             * We leave Minecraft's own fog colour untouched. This
             * allows the normal sky/fog colour relationship to remain
             * intact.
             */
            fog.start = &lrp->baseFogStart();
            fog.end = &lrp->baseFogEnd();
            fog.density = &lrp->currentFogDensityMax();

            fog.savedStart = *fog.start;
            fog.savedEnd = *fog.end;
            fog.savedDensity = *fog.density;

            fog.valid = true;

            const bool shouldApply =
                !g_chunkFadeMod->m_onlyThirdPerson ||
                g_chunkFadeMod->isThirdPersonActive();

            if (shouldApply) {
                const float fadeStart =
                    std::max(
                        0.0f,
                        g_chunkFadeMod->m_fadeStart);

                const float fadeEnd =
                    std::max(
                        fadeStart + 1.0f,
                        g_chunkFadeMod->m_fadeEnd);

                const float fadeDensity =
                    std::clamp(
                        g_chunkFadeMod->m_fadeOpacity,
                        0.0f,
                        1.0f);

                /*
                 * Only alter the distance/density parameters.
                 *
                 * Minecraft keeps ownership of the fog colour, so
                 * the distant terrain fades toward the same colour
                 * the game is already using for the atmosphere.
                 */
                *fog.start = fadeStart;
                *fog.end = fadeEnd;

                /*
                 * A zero density is useful for effectively disabling
                 * the additional fade. Otherwise use the requested
                 * fade density.
                 */
                *fog.density = fadeDensity;
            }
        }
    }

    if (_renderLevel_orig) {
        _renderLevel_orig(
            _this,
            screenContext,
            a3);
    }

    /*
     * Restore the exact values that existed before our temporary
     * render-time modification. This prevents the module from
     * permanently changing Minecraft's fog state.
     */
    if (fog.valid) {
        *fog.start = fog.savedStart;
        *fog.end = fog.savedEnd;
        *fog.density = fog.savedDensity;
    }
}

ChunkFadeModule::ChunkFadeModule()
    : Module(
        "Chunk Fade",
        "Smooths distant chunk transitions using Minecraft's existing fog colour.") {

    g_chunkFadeMod = this;
}

ChunkFadeModule::~ChunkFadeModule() {
    if (g_chunkFadeMod == this) {
        g_chunkFadeMod = nullptr;
    }
}

void ChunkFadeModule::onInit() {
    if (!m_perspectiveTarget) {
        const uintptr_t p =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::GetPerspective);

        if (p != 0) {
            m_perspectiveTarget =
                reinterpret_cast<void*>(p);
        }
    }

    if (!m_patchTarget) {
        const uintptr_t addr =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::RenderLevel);

        if (addr != 0) {
            m_patchTarget =
                reinterpret_cast<void*>(addr);
        }
    }

    if (m_perspectiveTarget &&
        !_getPerspective_orig) {

        bedrocktools::hooks::install(
            m_perspectiveTarget,
            reinterpret_cast<void*>(
                _getPerspective_hook),
            reinterpret_cast<void**>(
                &_getPerspective_orig));
    }
}

void ChunkFadeModule::onEnable() {
    if (m_patched ||
        !m_patchTarget) {
        return;
    }

    bedrocktools::hooks::install(
        m_patchTarget,
        reinterpret_cast<void*>(
            _renderLevel_hook),
        reinterpret_cast<void**>(
            &_renderLevel_orig));

    m_patched = true;
}

void ChunkFadeModule::onDisable() {
    /*
     * The hook restores all modified fog fields immediately after
     * each RenderLevel call, so disabling the module does not leave
     * modified fog values behind.
     */
}

void ChunkFadeModule::loadConfig(
    const nlohmann::json& j) {

    Module::loadConfig(j);

    if (j.contains("m_fadeStart")) {
        m_fadeStart =
            j["m_fadeStart"].get<float>();
    }

    if (j.contains("m_fadeEnd")) {
        m_fadeEnd =
            j["m_fadeEnd"].get<float>();
    }

    if (j.contains("m_fadeOpacity")) {
        m_fadeOpacity =
            j["m_fadeOpacity"].get<float>();
    }

    /*
     * Read old colour settings so existing configs remain valid,
     * but deliberately do not use them for rendering.
     */
    if (j.contains("m_fadeColorR")) {
        m_fadeColorR =
            j["m_fadeColorR"].get<float>();
    }

    if (j.contains("m_fadeColorG")) {
        m_fadeColorG =
            j["m_fadeColorG"].get<float>();
    }

    if (j.contains("m_fadeColorB")) {
        m_fadeColorB =
            j["m_fadeColorB"].get<float>();
    }

    if (j.contains("m_onlyThirdPerson")) {
        m_onlyThirdPerson =
            j["m_onlyThirdPerson"].get<bool>();
    }

    m_fadeStart =
        std::max(0.0f, m_fadeStart);

    m_fadeEnd =
        std::max(
            m_fadeStart + 1.0f,
            m_fadeEnd);

    m_fadeOpacity =
        std::clamp(
            m_fadeOpacity,
            0.0f,
            1.0f);
}

void ChunkFadeModule::saveConfig(
    nlohmann::json& j) {

    Module::saveConfig(j);

    j["m_fadeStart"] = m_fadeStart;
    j["m_fadeEnd"] = m_fadeEnd;
    j["m_fadeOpacity"] = m_fadeOpacity;

    /*
     * Keep these fields in the config for backwards compatibility,
     * even though the renderer no longer uses them.
     */
    j["m_fadeColorR"] = m_fadeColorR;
    j["m_fadeColorG"] = m_fadeColorG;
    j["m_fadeColorB"] = m_fadeColorB;

    j["m_onlyThirdPerson"] =
        m_onlyThirdPerson;
}
