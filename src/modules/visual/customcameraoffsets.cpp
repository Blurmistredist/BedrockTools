#include "customcameraoffsets.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>

#include <EGL/egl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

static CustomCameraOffsetsModule* g_cameraMod = nullptr;

using RenderLevelFn =
    void (*)(void*, void*, void*);

using GetPerspectiveFn =
    int (*)(void*);

using HudCursorRenderFn =
    void (*)(void*, void*, void*, void*);

using IsSolidBlockingBlockFn =
    bool (*)(void*, const bedrocktools::sdk::BlockPos*);

static RenderLevelFn s_renderLevelOrig = nullptr;
static GetPerspectiveFn s_getPerspectiveOrig = nullptr;
static HudCursorRenderFn s_hudCursorOrig = nullptr;

static IsSolidBlockingBlockFn s_isSolidBlockingBlock = nullptr;

static bedrocktools::sdk::Vec3 addVec(
    const bedrocktools::sdk::Vec3& a,
    const bedrocktools::sdk::Vec3& b) {

    return {
        a.x + b.x,
        a.y + b.y,
        a.z + b.z
    };
}

static bedrocktools::sdk::Vec3 subVec(
    const bedrocktools::sdk::Vec3& a,
    const bedrocktools::sdk::Vec3& b) {

    return {
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    };
}

static bedrocktools::sdk::Vec3 mulVec(
    const bedrocktools::sdk::Vec3& a,
    float s) {

    return {
        a.x * s,
        a.y * s,
        a.z * s
    };
}

static float lengthVec(
    const bedrocktools::sdk::Vec3& v) {

    return std::sqrt(
        v.x * v.x +
        v.y * v.y +
        v.z * v.z);
}

static bedrocktools::sdk::Vec3 normalizeVec(
    const bedrocktools::sdk::Vec3& v) {

    const float len = lengthVec(v);

    if (len < 0.0001f) {
        return {0.0f, 0.0f, 0.0f};
    }

    return mulVec(v, 1.0f / len);
}

static bedrocktools::sdk::Vec3 lerpVec(
    const bedrocktools::sdk::Vec3& a,
    const bedrocktools::sdk::Vec3& b,
    float t) {

    t = std::clamp(t, 0.0f, 1.0f);

    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

static bool isBlocked(
    bedrocktools::sdk::BlockSource* region,
    const bedrocktools::sdk::Vec3& point) {

    if (!region || !s_isSolidBlockingBlock) {
        return false;
    }

    const bedrocktools::sdk::BlockPos pos{
        static_cast<int>(std::floor(point.x)),
        static_cast<int>(std::floor(point.y)),
        static_cast<int>(std::floor(point.z))
    };

    return s_isSolidBlockingBlock(region, &pos);
}

static int s_getPerspectiveHook(void* self) {
    int result = 0;

    if (s_getPerspectiveOrig) {
        result = s_getPerspectiveOrig(self);
    }

    if (g_cameraMod) {
        g_cameraMod->setThirdPersonState(result != 0);
    }

    return result;
}

static void s_hudCursorHook(
    void* self,
    void* a1,
    void* a2,
    void* a3) {

    // Suppress vanilla's cursor only while our forced crosshair is
    // active. The module draws its own fixed center reticle.
    if (g_cameraMod &&
        g_cameraMod->enabled &&
        g_cameraMod->m_forceCrosshair) {
        return;
    }

    if (s_hudCursorOrig) {
        s_hudCursorOrig(
            self,
            a1,
            a2,
            a3);
    }
}

static void s_renderLevelHook(
    void* self,
    void* screenContext,
    void* a3) {

    struct Restore {
        bedrocktools::sdk::LevelRendererPlayer* renderer = nullptr;
        bedrocktools::sdk::Vec3 original{};
        bool valid = false;
    } restore;

    if (g_cameraMod &&
        g_cameraMod->enabled) {

        auto* levelRenderer =
            reinterpret_cast<
                bedrocktools::sdk::LevelRenderer*>(self);

        auto* renderer =
            levelRenderer
                ? levelRenderer->playerRenderer()
                : nullptr;

        auto* client =
            bedrocktools::sdk::ClientInstance::current();

        auto* player =
            client
                ? client->localPlayer()
                : nullptr;

        auto* region =
            client
                ? client->region()
                : nullptr;

        if (renderer &&
            player &&
            (!g_cameraMod->m_onlyThirdPerson ||
             g_cameraMod->isThirdPersonActive())) {

            restore.renderer = renderer;
            restore.original =
                renderer->cameraPosition();
            restore.valid = true;

            const auto playerPos =
                player->position();

            const auto playerRotation =
                player->rotation();

            const auto target =
                g_cameraMod->calculateCameraPosition(
                    playerPos,
                    playerRotation,
                    region);

            bedrocktools::sdk::Vec3 finalCamera =
                target;

            if (g_cameraMod->m_hasLastCamera) {
                finalCamera =
                    lerpVec(
                        g_cameraMod->m_lastCamera,
                        target,
                        g_cameraMod->m_positionSmoothing);
            }

            g_cameraMod->m_lastCamera =
                finalCamera;

            g_cameraMod->m_hasLastCamera =
                true;

            renderer->cameraPosition() =
                finalCamera;
        }
    }

    if (s_renderLevelOrig) {
        s_renderLevelOrig(
            self,
            screenContext,
            a3);
    }

    if (restore.valid) {
        restore.renderer->cameraPosition() =
            restore.original;
    }
}

CustomCameraOffsetsModule::CustomCameraOffsetsModule()
    : Module(
        "Custom Camera Offsets",
        "Dynamic third-person camera with block-safe positioning and a fixed center crosshair.") {

    g_cameraMod = this;

    hideInHudEditor = true;
}

CustomCameraOffsetsModule::~CustomCameraOffsetsModule() {
    if (g_cameraMod == this) {
        g_cameraMod = nullptr;
    }
}

bool CustomCameraOffsetsModule::isThirdPerson() const {
    return m_thirdPerson;
}

void CustomCameraOffsetsModule::onInit() {
    if (!m_perspectiveTarget) {
        const uintptr_t address =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    GetPerspective);

        if (address) {
            m_perspectiveTarget =
                reinterpret_cast<void*>(address);
        }
    }

    if (!m_renderTarget) {
        const uintptr_t address =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    RenderLevel);

        if (address) {
            m_renderTarget =
                reinterpret_cast<void*>(address);
        }
    }

    if (!m_cursorTarget) {
        const uintptr_t address =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    HudCursor);

        if (address) {
            m_cursorTarget =
                reinterpret_cast<void*>(address);
        }
    }

    if (!s_isSolidBlockingBlock) {
        const uintptr_t address =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    BlockSourceIsSolidBlockingBlock);

        if (address) {
            s_isSolidBlockingBlock =
                reinterpret_cast<
                    IsSolidBlockingBlockFn>(address);
        }
    }

    if (m_perspectiveTarget &&
        !m_perspectiveHooked) {

        bedrocktools::hooks::install(
            m_perspectiveTarget,
            reinterpret_cast<void*>(
                s_getPerspectiveHook),
            reinterpret_cast<void**>(
                &s_getPerspectiveOrig));

        m_perspectiveHooked = true;
    }

    if (m_cursorTarget &&
        !m_cursorHooked) {

        bedrocktools::hooks::install(
            m_cursorTarget,
            reinterpret_cast<void*>(
                s_hudCursorHook),
            reinterpret_cast<void**>(
                &s_hudCursorOrig));

        m_cursorHooked = true;
    }
}

void CustomCameraOffsetsModule::onEnable() {
    m_hasLastCamera = false;

    if (m_renderTarget &&
        !m_renderHooked) {

        bedrocktools::hooks::install(
            m_renderTarget,
            reinterpret_cast<void*>(
                s_renderLevelHook),
            reinterpret_cast<void**>(
                &s_renderLevelOrig));

        m_renderHooked = true;
    }
}

void CustomCameraOffsetsModule::onDisable() {
    m_hasLastCamera = false;
}

bedrocktools::sdk::Vec3
CustomCameraOffsetsModule::resolveCameraCollision(
    const bedrocktools::sdk::Vec3& origin,
    const bedrocktools::sdk::Vec3& target,
    bedrocktools::sdk::BlockSource* region) const {

    if (!region || !s_isSolidBlockingBlock) {
        return target;
    }

    const auto delta =
        subVec(target, origin);

    const float distance =
        lengthVec(delta);

    if (distance <= 0.001f) {
        return target;
    }

    const auto direction =
        normalizeVec(delta);

    const float maxDistance =
        std::clamp(
            distance,
            0.0f,
            m_maxDistance);

    // Fine stepping prevents the camera from skipping through a
    // one-block obstruction.
    constexpr float step = 0.08f;

    float lastSafe = 0.0f;

    for (float d = step;
         d <= maxDistance;
         d += step) {

        const auto point =
            addVec(
                origin,
                mulVec(direction, d));

        bool blocked = false;

        // A small camera-volume approximation rather than a single
        // point check.
        constexpr float samples[] = {
            0.0f,
            0.16f,
            -0.16f
        };

        for (const float dy : samples) {
            const auto sample =
                bedrocktools::sdk::Vec3{
                    point.x,
                    point.y + dy,
                    point.z
                };

            if (isBlocked(region, sample)) {
                blocked = true;
                break;
            }
        }

        if (blocked) {
            return addVec(
                origin,
                mulVec(
                    direction,
                    std::max(
                        0.0f,
                        lastSafe - 0.06f)));
        }

        lastSafe = d;
    }

    return addVec(
        origin,
        mulVec(direction, maxDistance));
}

bedrocktools::sdk::Vec3
CustomCameraOffsetsModule::calculateCameraPosition(
    const bedrocktools::sdk::Vec3& playerPos,
    const bedrocktools::sdk::Vec2& playerRotation,
    bedrocktools::sdk::BlockSource* region) const {

    constexpr float PI =
        3.14159265358979323846f;

    const float yaw =
        (180.0f +
         playerRotation.y +
         m_yawOffset) *
        (PI / 180.0f);

    const float pitch =
        -(playerRotation.x +
          m_pitchOffset) *
        (PI / 180.0f);

    const float cosYaw =
        std::cos(yaw);

    const float sinYaw =
        std::sin(yaw);

    const float cosPitch =
        std::cos(pitch);

    const float sinPitch =
        std::sin(pitch);

    // Player look vector. This keeps the camera's dynamic motion
    // coupled to the same live look direction used by the player.
    const bedrocktools::sdk::Vec3 forward{
        -sinYaw * cosPitch,
        sinPitch,
        cosYaw * cosPitch
    };

    const bedrocktools::sdk::Vec3 right{
        cosYaw,
        0.0f,
        sinYaw
    };

    const bedrocktools::sdk::Vec3 origin{
        playerPos.x,
        playerPos.y + m_height,
        playerPos.z
    };

    float distance =
        std::clamp(
            m_distance,
            m_minDistance,
            m_maxDistance);

    /*
     * Dynamic third-person context:
     *
     * When the player pitches strongly upward/downward, keep the
     * camera from making an excessively large vertical excursion.
     * The camera therefore remains a stable third-person boom while
     * still following the player's live look direction.
     */
    const float verticalFactor =
        std::clamp(
            cosPitch,
            0.35f,
            1.0f);

    if (m_dynamicDistance) {
        distance =
            std::clamp(
                distance *
                    (0.88f +
                     0.12f * verticalFactor),
                m_minDistance,
                m_maxDistance);
    }

    // Camera sits behind the player's look direction.
    // Shoulder offset is perpendicular to that direction.
    const bedrocktools::sdk::Vec3 requested{
        origin.x +
            right.x * m_shoulderX -
            forward.x * distance,

        origin.y -
            forward.y * distance,

        origin.z +
            right.z * m_shoulderX -
            forward.z * distance
    };

    return resolveCameraCollision(
        origin,
        requested,
        region);
}

void CustomCameraOffsetsModule::updateCamera() {
    // Render-time camera placement is performed by the RenderLevel
    // hook. This function intentionally remains lightweight.
}

void CustomCameraOffsetsModule::drawCrosshair() {
    if (!enabled || !m_forceCrosshair) {
        return;
    }

    EGLDisplay display =
        eglGetCurrentDisplay();

    EGLSurface surface =
        eglGetCurrentSurface(
            EGL_DRAW);

    if (display == EGL_NO_DISPLAY ||
        surface == EGL_NO_SURFACE) {
        return;
    }

    EGLint width = 0;
    EGLint height = 0;

    if (!eglQuerySurface(
            display,
            surface,
            EGL_WIDTH,
            &width) ||
        !eglQuerySurface(
            display,
            surface,
            EGL_HEIGHT,
            &height) ||
        width <= 0 ||
        height <= 0) {
        return;
    }

    const float cx =
        static_cast<float>(width) * 0.5f;

    const float cy =
        static_cast<float>(height) * 0.5f;

    const float size =
        std::max(1.0f, m_crosshairSize);

    const float gap =
        std::max(0.0f, m_crosshairGap);

    const float thickness =
        std::max(1.0f, m_crosshairThickness);

    std::vector<PLModMenu_DrawCommand> commands;
    commands.reserve(5);

    const uint32_t outline =
        0xC8000000;

    const uint32_t white =
        0xFFFFFFFF;

    auto addLine =
        [&](float x1,
            float y1,
            float x2,
            float y2,
            float lineSize,
            uint32_t color) {

            PLModMenu_DrawCommand cmd = {};
            cmd.type = PL_DRAW_LINE;
            cmd.x = x1;
            cmd.y = y1;
            cmd.w = x2 - x1;
            cmd.h = y2 - y1;
            cmd.size = lineSize;
            cmd.color = color;

            commands.push_back(cmd);
        };

    // Four-segment center reticle. The black underlay remains visible
    // over both bright terrain and the sky.
    addLine(
        cx - gap - size,
        cy,
        cx - gap,
        cy,
        thickness + 2.0f,
        outline);

    addLine(
        cx + gap,
        cy,
        cx + gap + size,
        cy,
        thickness + 2.0f,
        outline);

    addLine(
        cx,
        cy - gap - size,
        cx,
        cy - gap,
        thickness + 2.0f,
        outline);

    addLine(
        cx,
        cy + gap,
        cx,
        cy + gap + size,
        thickness + 2.0f,
        outline);

    addLine(
        cx - gap - size,
        cy,
        cx - gap,
        cy,
        thickness,
        white);

    addLine(
        cx + gap,
        cy,
        cx + gap + size,
        cy,
        thickness,
        white);

    addLine(
        cx,
        cy - gap - size,
        cx,
        cy - gap,
        thickness,
        white);

    addLine(
        cx,
        cy + gap,
        cx,
        cy + gap + size,
        thickness,
        white);

    submitDrawCommands(
        moduleId,
        commands);
}

void CustomCameraOffsetsModule::onFrame() {
    if (!enabled) {
        return;
    }

    drawCrosshair();
}

void CustomCameraOffsetsModule::loadConfig(
    const nlohmann::json& j) {

    Module::loadConfig(j);

    if (j.contains("m_distance"))
        m_distance =
            j["m_distance"].get<float>();

    if (j.contains("m_minDistance"))
        m_minDistance =
            j["m_minDistance"].get<float>();

    if (j.contains("m_maxDistance"))
        m_maxDistance =
            j["m_maxDistance"].get<float>();

    if (j.contains("m_shoulderX"))
        m_shoulderX =
            j["m_shoulderX"].get<float>();

    if (j.contains("m_height"))
        m_height =
            j["m_height"].get<float>();

    if (j.contains("m_yawOffset"))
        m_yawOffset =
            j["m_yawOffset"].get<float>();

    if (j.contains("m_pitchOffset"))
        m_pitchOffset =
            j["m_pitchOffset"].get<float>();

    if (j.contains("m_positionSmoothing"))
        m_positionSmoothing =
            j["m_positionSmoothing"].get<float>();

    if (j.contains("m_onlyThirdPerson"))
        m_onlyThirdPerson =
            j["m_onlyThirdPerson"].get<bool>();

    if (j.contains("m_dynamicDistance"))
        m_dynamicDistance =
            j["m_dynamicDistance"].get<bool>();

    if (j.contains("m_forceCrosshair"))
        m_forceCrosshair =
            j["m_forceCrosshair"].get<bool>();

    if (j.contains("m_crosshairSize"))
        m_crosshairSize =
            j["m_crosshairSize"].get<float>();

    if (j.contains("m_crosshairThickness"))
        m_crosshairThickness =
            j["m_crosshairThickness"].get<float>();

    if (j.contains("m_crosshairGap"))
        m_crosshairGap =
            j["m_crosshairGap"].get<float>();

    m_minDistance =
        std::max(0.25f, m_minDistance);

    m_maxDistance =
        std::max(
            m_minDistance + 0.25f,
            m_maxDistance);

    m_distance =
        std::clamp(
            m_distance,
            m_minDistance,
            m_maxDistance);

    m_positionSmoothing =
        std::clamp(
            m_positionSmoothing,
            0.0f,
            1.0f);

    m_crosshairSize =
        std::max(
            1.0f,
            m_crosshairSize);

    m_crosshairThickness =
        std::max(
            1.0f,
            m_crosshairThickness);

    m_crosshairGap =
        std::max(
            0.0f,
            m_crosshairGap);
}

void CustomCameraOffsetsModule::saveConfig(
    nlohmann::json& j) {

    Module::saveConfig(j);

    j["m_distance"] =
        m_distance;

    j["m_minDistance"] =
        m_minDistance;

    j["m_maxDistance"] =
        m_maxDistance;

    j["m_shoulderX"] =
        m_shoulderX;

    j["m_height"] =
        m_height;

    j["m_yawOffset"] =
        m_yawOffset;

    j["m_pitchOffset"] =
        m_pitchOffset;

    j["m_positionSmoothing"] =
        m_positionSmoothing;

    j["m_onlyThirdPerson"] =
        m_onlyThirdPerson;

    j["m_dynamicDistance"] =
        m_dynamicDistance;

    j["m_forceCrosshair"] =
        m_forceCrosshair;

    j["m_crosshairSize"] =
        m_crosshairSize;

    j["m_crosshairThickness"] =
        m_crosshairThickness;

    j["m_crosshairGap"] =
        m_crosshairGap;
}
