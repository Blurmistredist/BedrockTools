#include "customcameraoffsets.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/FrameEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
CustomCameraOffsetsModule* g_camera = nullptr;

void (*g_renderLevelOriginal)(void*, void*, void*) = nullptr;
int (*g_getPerspectiveOriginal)(void*) = nullptr;

bool g_forceThirdPerson = false;
bool g_currentThirdPerson = false;

struct BlockPosRaw {
    int32_t x;
    int32_t y;
    int32_t z;
};
using IsSolidBlockingFn = bool (*)(void*, const BlockPosRaw*);

IsSolidBlockingFn g_isSolidBlocking = nullptr;

float degToRad(float value) {
    return value * 3.14159265358979323846f / 180.0f;
}

float length(const bedrocktools::sdk::Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

bedrocktools::sdk::Vec3 normalize(const bedrocktools::sdk::Vec3& v) {
    const float len = length(v);
    if (len <= 0.0001f) return {};
    return {v.x / len, v.y / len, v.z / len};
}

int perspectiveHook(void* self) {
    const int original = g_getPerspectiveOriginal
        ? g_getPerspectiveOriginal(self)
        : 0;

    g_currentThirdPerson = (original != 0);
    if (g_forceThirdPerson)
        return 1;

    return original;
}

bool isSolid(void* region, const bedrocktools::sdk::Vec3& p) {
    if (!region || !g_isSolidBlocking) return false;

    BlockPosRaw pos{
        static_cast<int32_t>(std::floor(p.x)),
        static_cast<int32_t>(std::floor(p.y)),
        static_cast<int32_t>(std::floor(p.z))
    };

    return g_isSolidBlocking(region, &pos);
}

void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (!g_renderLevelOriginal) return;

    if (!g_camera || !g_camera->enabled || !self ||
        (!g_forceThirdPerson && !g_currentThirdPerson)) {
        g_renderLevelOriginal(self, screenContext, a3);
        return;
    }

    auto* client = bedrocktools::sdk::ClientInstance::current();
    auto* player = client ? client->localPlayer() : nullptr;
    auto* region = client ? client->region() : nullptr;

    if (!player || !region) {
        g_renderLevelOriginal(self, screenContext, a3);
        return;
    }

    auto* rendererPlayer = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(self) +
        bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);

    if (!rendererPlayer) {
        g_renderLevelOriginal(self, screenContext, a3);
        return;
    }

    const uintptr_t camPosAddress =
        reinterpret_cast<uintptr_t>(rendererPlayer) +
        bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos;

    auto& currentCamera =
        *reinterpret_cast<bedrocktools::sdk::Vec3*>(camPosAddress);

    const auto playerPos = player->position();
    const auto rotation = player->rotation();

    auto desired = g_camera->calculateCameraPosition(
        playerPos, rotation, region);

    if (g_camera->collision())
        desired = g_camera->resolveCollision(
            {playerPos.x, playerPos.y + 1.62f, playerPos.z},
            desired,
            region);

    if (!g_camera->dynamic()) {
        g_camera->smoothedCamera() = desired;
        g_camera->setHasSmoothedCamera(true);
    } else if (!g_camera->hasSmoothedCamera()) {
        g_camera->smoothedCamera() = desired;
        g_camera->setHasSmoothedCamera(true);
    } else {
        // RenderLevel can be called more than once per frame. A bounded
        // interpolation keeps the camera responsive without teleporting.
        const float alpha = std::clamp(
            1.0f - std::exp(-g_camera->smoothness() / 60.0f),
            0.0f, 1.0f);

        g_camera->smoothedCamera().x +=
            (desired.x - g_camera->smoothedCamera().x) * alpha;
        g_camera->smoothedCamera().y +=
            (desired.y - g_camera->smoothedCamera().y) * alpha;
        g_camera->smoothedCamera().z +=
            (desired.z - g_camera->smoothedCamera().z) * alpha;
    }

    const auto saved = currentCamera;
    currentCamera = g_camera->smoothedCamera();

    g_renderLevelOriginal(self, screenContext, a3);

    currentCamera = saved;
}

void crosshairLine(std::vector<PLModMenu_DrawCommand>& cmds,
                   float x, float y, float w, float h,
                   float thickness, std::uint32_t color) {
    PLModMenu_DrawCommand c{};
    c.type = PL_DRAW_LINE;
    c.x = x;
    c.y = y;
    c.w = w;
    c.h = h;
    c.size = thickness;
    c.color = color;
    cmds.push_back(c);
}
}

CustomCameraOffsetsModule::CustomCameraOffsetsModule()
    : Module("Custom Camera Offsets", "Dynamic third-person camera with collision-safe offsets and a forced crosshair.") {
    g_camera = this;
    hideInHudEditor = true;
}

CustomCameraOffsetsModule::~CustomCameraOffsetsModule() {
    if (g_camera == this) g_camera = nullptr;
}

void CustomCameraOffsetsModule::onInit() {
    if (!m_renderHooked) {
        const auto addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderLevel);
        if (addr) {
            m_renderPatchTarget = reinterpret_cast<void*>(addr);
            m_renderHooked = static_cast<bool>(bedrocktools::hooks::install(
                m_renderPatchTarget,
                reinterpret_cast<void*>(renderLevelHook),
                reinterpret_cast<void**>(&g_renderLevelOriginal)));
        }
    }

    if (!m_perspectiveHooked) {
        const auto addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::GetPerspective);
        if (addr) {
            m_perspectiveHooked = static_cast<bool>(bedrocktools::hooks::install(
                reinterpret_cast<void*>(addr),
                reinterpret_cast<void*>(perspectiveHook),
                reinterpret_cast<void**>(&g_getPerspectiveOriginal)));
        }
    }

    if (!g_isSolidBlocking) {
        const auto addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
        if (addr)
            g_isSolidBlocking = reinterpret_cast<IsSolidBlockingFn>(addr);
    }
}

void CustomCameraOffsetsModule::onEnable() {
    m_haveSmoothedCamera = false;
    g_forceThirdPerson = m_forceThirdPerson;
}

void CustomCameraOffsetsModule::onDisable() {
    g_forceThirdPerson = false;
    m_haveSmoothedCamera = false;
}

bedrocktools::sdk::Vec3 CustomCameraOffsetsModule::calculateCameraPosition(
    const bedrocktools::sdk::Vec3& playerPos,
    const bedrocktools::sdk::Vec2& rotation,
    void*) const {

    // Actor::rotation() in this SDK stores yaw at x and pitch at y.
    const float yaw = degToRad(rotation.x);

    // Horizontal forward vector. The camera is placed behind it.
    const bedrocktools::sdk::Vec3 forward{
        -std::sin(yaw),
        0.0f,
        std::cos(yaw)
    };

    const bedrocktools::sdk::Vec3 right{
        std::cos(yaw),
        0.0f,
        std::sin(yaw)
    };

    const float distance = std::max(0.25f, m_distance);

    return {
        playerPos.x + right.x * m_sideOffset - forward.x * distance,
        playerPos.y + 1.62f + m_verticalOffset,
        playerPos.z + right.z * m_sideOffset - forward.z * distance
    };
}

bedrocktools::sdk::Vec3 CustomCameraOffsetsModule::resolveCollision(
    const bedrocktools::sdk::Vec3& start,
    const bedrocktools::sdk::Vec3& desired,
    void* region) const {

    const auto delta = bedrocktools::sdk::Vec3{
        desired.x - start.x,
        desired.y - start.y,
        desired.z - start.z
    };

    const float distance = length(delta);
    if (distance <= 0.1f) return desired;

    const auto direction = normalize(delta);

    // Sample the camera ray. We stop just before the first solid block.
    const int samples = std::clamp(
        static_cast<int>(std::ceil(distance * 8.0f)), 8, 160);

    bedrocktools::sdk::Vec3 lastSafe = start;

    for (int i = 1; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const auto point = bedrocktools::sdk::Vec3{
            start.x + delta.x * t,
            start.y + delta.y * t,
            start.z + delta.z * t
        };

        if (isSolid(region, point)) {
            // Pull back slightly from the collision cell so the camera
            // never intentionally enters a solid block.
            const float pullback = 0.18f;
            return {
                lastSafe.x - direction.x * pullback,
                lastSafe.y - direction.y * pullback,
                lastSafe.z - direction.z * pullback
            };
        }

        lastSafe = point;
    }

    return desired;
}

void CustomCameraOffsetsModule::onFrame() {
    if (!enabled || !m_crosshair) return;
    drawCrosshair();
}

void CustomCameraOffsetsModule::drawCrosshair() {
    std::vector<PLModMenu_DrawCommand> cmds;

    const float s = std::clamp(m_crosshairSize, 2.0f, 30.0f);
    const float t = std::clamp(m_crosshairThickness, 0.5f, 8.0f);

    // -20000 is the ModMenu "screen centre" anchor used by the existing
    // BedrockTools HUD renderer.
    const float cx = -20000.0f;
    const float cy = -20000.0f;

    crosshairLine(cmds, cx - s, cy, s * 2.0f, 0.0f, t, 0xFF101010u);
    crosshairLine(cmds, cx, cy - s, 0.0f, s * 2.0f, t, 0xFF101010u);

    const float inner = std::max(1.0f, t * 0.55f);
    crosshairLine(cmds, cx - s, cy, s * 2.0f, 0.0f, inner, 0xFFFFFFFFu);
    crosshairLine(cmds, cx, cy - s, 0.0f, s * 2.0f, inner, 0xFFFFFFFFu);

    submitDrawCommands(moduleId, cmds);
}

void CustomCameraOffsetsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_forceThirdPerson = j.value("m_forceThirdPerson", m_forceThirdPerson);
    m_collision = j.value("m_collision", m_collision);
    m_crosshair = j.value("m_crosshair", m_crosshair);
    m_dynamic = j.value("m_dynamic", m_dynamic);
    m_distance = j.value("m_distance", m_distance);
    m_sideOffset = j.value("m_sideOffset", m_sideOffset);
    m_verticalOffset = j.value("m_verticalOffset", m_verticalOffset);
    m_smoothness = j.value("m_smoothness", m_smoothness);
    m_crosshairSize = j.value("m_crosshairSize", m_crosshairSize);
    m_crosshairThickness = j.value("m_crosshairThickness", m_crosshairThickness);
}

void CustomCameraOffsetsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_forceThirdPerson"] = m_forceThirdPerson;
    j["m_collision"] = m_collision;
    j["m_crosshair"] = m_crosshair;
    j["m_dynamic"] = m_dynamic;
    j["m_distance"] = m_distance;
    j["m_sideOffset"] = m_sideOffset;
    j["m_verticalOffset"] = m_verticalOffset;
    j["m_smoothness"] = m_smoothness;
    j["m_crosshairSize"] = m_crosshairSize;
    j["m_crosshairThickness"] = m_crosshairThickness;
}
