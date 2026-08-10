#include "customcameraoffsets.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/ClientInstance.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <cmath>

static CustomCameraOffsetsModule* g_cameraMod = nullptr;
static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3) = nullptr;
static int (*_getPerspective_orig)(void* _this) = nullptr;

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

bool CustomCameraOffsetsModule::isThirdPerson() const {
    return m_thirdPerson;
}

static int _getPerspective_hook(void* _this) {
    int result = 0;
    if (_getPerspective_orig) {
        result = _getPerspective_orig(_this);
    }
    if (g_cameraMod) {
        g_cameraMod->m_thirdPerson = (result != 0);
    }
    return result;
}

static bedrocktools::sdk::Vec3 buildOffset(const bedrocktools::sdk::Vec3& playerPos,
                                           const bedrocktools::sdk::Vec2& rotation,
                                           const CustomCameraOffsetsModule& mod) {
    if (!mod.m_useLookDirection) {
        return {
            playerPos.x + mod.m_offsetX,
            playerPos.y + mod.m_offsetY,
            playerPos.z + mod.m_offsetZ,
        };
    }

    const float PI = 3.14159265358979323846f;
    const float yaw = (180.0f + rotation.y + mod.m_yawOffset) * (PI / 180.0f);
    const float pitch = (-(rotation.x + mod.m_pitchOffset)) * (PI / 180.0f);

    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);

    const bedrocktools::sdk::Vec3 forward {
        -sinYaw * cosPitch,
        sinPitch,
        cosYaw * cosPitch,
    };

    const bedrocktools::sdk::Vec3 right {
        cosYaw,
        0.0f,
        sinYaw,
    };

    const bedrocktools::sdk::Vec3 up { 0.0f, 1.0f, 0.0f };

    return {
        playerPos.x + right.x * mod.m_offsetX + up.x * mod.m_offsetY + forward.x * mod.m_offsetZ,
        playerPos.y + right.y * mod.m_offsetX + up.y * mod.m_offsetY + forward.y * mod.m_offsetZ,
        playerPos.z + right.z * mod.m_offsetX + up.z * mod.m_offsetY + forward.z * mod.m_offsetZ,
    };
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    struct CameraRestore {
        bedrocktools::sdk::LevelRendererPlayer* lrp = nullptr;
        bedrocktools::sdk::Vec3 original{};
        bool valid = false;
    } restore;

    if (g_cameraMod && g_cameraMod->enabled) {
        auto* client = bedrocktools::sdk::ClientInstance::current();
        auto* player = client ? client->localPlayer() : nullptr;
        auto* levelRenderer = reinterpret_cast<bedrocktools::sdk::LevelRenderer*>(_this);
        auto* lrp = levelRenderer ? levelRenderer->playerRenderer() : nullptr;

        if (player && lrp && (!g_cameraMod->m_onlyThirdPerson || g_cameraMod->m_thirdPerson)) {
            restore.lrp = lrp;
            restore.original = lrp->cameraPosition();
            restore.valid = true;

            const auto playerPos = player->position();
            const auto rotation = player->rotation();
            bedrocktools::sdk::Vec3 target = buildOffset(playerPos, rotation, *g_cameraMod);

            if (g_cameraMod->m_hasLastCamera) {
                target = lerpVec(g_cameraMod->m_lastCamera, target, g_cameraMod->m_smoothness);
            }
            g_cameraMod->m_lastCamera = target;
            g_cameraMod->m_hasLastCamera = true;
            lrp->cameraPosition() = target;
        }
    }

    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (restore.valid) {
        restore.lrp->cameraPosition() = restore.original;
    }
}

CustomCameraOffsetsModule::CustomCameraOffsetsModule()
    : Module("Custom Camera Offsets", "Moves the third-person camera with configurable offsets.") {
    g_cameraMod = this;
}

CustomCameraOffsetsModule::~CustomCameraOffsetsModule() {
    if (g_cameraMod == this) g_cameraMod = nullptr;
}

void CustomCameraOffsetsModule::onInit() {
    if (!m_perspectiveTarget) {
        const uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetPerspective);
        if (addr != 0) m_perspectiveTarget = reinterpret_cast<void*>(addr);
    }
    if (!m_patchTarget) {
        const uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
        if (addr != 0) m_patchTarget = reinterpret_cast<void*>(addr);
    }

    if (m_perspectiveTarget && !_getPerspective_orig) {
        bedrocktools::hooks::install(m_perspectiveTarget, reinterpret_cast<void*>(_getPerspective_hook), reinterpret_cast<void**>(&_getPerspective_orig));
    }
}

void CustomCameraOffsetsModule::onEnable() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, reinterpret_cast<void*>(_renderLevel_hook), reinterpret_cast<void**>(&_renderLevel_orig));
    m_patched = true;
}

void CustomCameraOffsetsModule::onDisable() {
    m_hasLastCamera = false;
}

void CustomCameraOffsetsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_offsetX")) m_offsetX = j["m_offsetX"].get<float>();
    if (j.contains("m_offsetY")) m_offsetY = j["m_offsetY"].get<float>();
    if (j.contains("m_offsetZ")) m_offsetZ = j["m_offsetZ"].get<float>();
    if (j.contains("m_yawOffset")) m_yawOffset = j["m_yawOffset"].get<float>();
    if (j.contains("m_pitchOffset")) m_pitchOffset = j["m_pitchOffset"].get<float>();
    if (j.contains("m_smoothness")) m_smoothness = j["m_smoothness"].get<float>();
    if (j.contains("m_onlyThirdPerson")) m_onlyThirdPerson = j["m_onlyThirdPerson"].get<bool>();
    if (j.contains("m_useLookDirection")) m_useLookDirection = j["m_useLookDirection"].get<bool>();
    m_smoothness = std::clamp(m_smoothness, 0.0f, 1.0f);
}

void CustomCameraOffsetsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_offsetX"] = m_offsetX;
    j["m_offsetY"] = m_offsetY;
    j["m_offsetZ"] = m_offsetZ;
    j["m_yawOffset"] = m_yawOffset;
    j["m_pitchOffset"] = m_pitchOffset;
    j["m_smoothness"] = m_smoothness;
    j["m_onlyThirdPerson"] = m_onlyThirdPerson;
    j["m_useLookDirection"] = m_useLookDirection;
}
