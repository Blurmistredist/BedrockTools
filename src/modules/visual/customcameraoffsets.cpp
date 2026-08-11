#include "customcameraoffsets.hpp"

#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

struct BlockPosRaw {
    int32_t x;
    int32_t y;
    int32_t z;
};

using BlockSourceIsSolidBlockingBlockFn =
    bool (*)(void*, const BlockPosRaw*);

static CustomCameraOffsetsModule* g_cameraMod = nullptr;

static BlockSourceIsSolidBlockingBlockFn
    g_isSolidBlockingBlock = nullptr;

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

static bedrocktools::sdk::Vec3 add(
    const bedrocktools::sdk::Vec3& a,
    const bedrocktools::sdk::Vec3& b) {

    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static bedrocktools::sdk::Vec3 sub(
    const bedrocktools::sdk::Vec3& a,
    const bedrocktools::sdk::Vec3& b) {

    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static bedrocktools::sdk::Vec3 mul(
    const bedrocktools::sdk::Vec3& v,
    float s) {

    return {v.x * s, v.y * s, v.z * s};
}

static float length(
    const bedrocktools::sdk::Vec3& v) {

    return std::sqrt(
        v.x * v.x +
        v.y * v.y +
        v.z * v.z);
}

static bedrocktools::sdk::Vec3 normalize(
    const bedrocktools::sdk::Vec3& v) {

    const float len = length(v);

    if (len <= 0.00001f) {
        return {0.0f, 0.0f, 0.0f};
    }

    return mul(v, 1.0f / len);
}

static bedrocktools::sdk::Vec3 buildOffset(
    const bedrocktools::sdk::Vec3& playerPos,
    const bedrocktools::sdk::Vec2& rotation,
    const CustomCameraOffsetsModule& mod) {

    if (!mod.m_useLookDirection) {
        return {
            playerPos.x + mod.m_offsetX,
            playerPos.y + mod.m_offsetY,
            playerPos.z + mod.m_offsetZ
        };
    }

    constexpr float PI = 3.14159265358979323846f;

    const float yaw =
        (180.0f + rotation.y + mod.m_yawOffset) *
        (PI / 180.0f);

    const float pitch =
        (-(rotation.x + mod.m_pitchOffset)) *
        (PI / 180.0f);

    const float cosYaw = std::cos(yaw);
    const float sinYaw = std::sin(yaw);
    const float cosPitch = std::cos(pitch);
    const float sinPitch = std::sin(pitch);

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

    return {
        playerPos.x +
            right.x * mod.m_offsetX +
            forward.x * mod.m_offsetZ,

        playerPos.y +
            mod.m_offsetY +
            forward.y * mod.m_offsetZ,

        playerPos.z +
            right.z * mod.m_offsetX +
            forward.z * mod.m_offsetZ
    };
}

} // namespace

CustomCameraOffsetsModule::CustomCameraOffsetsModule()
    : Module(
        "Custom Camera Offsets",
        "Moves the third-person camera while preserving block collision.") {

    g_cameraMod = this;
}

CustomCameraOffsetsModule::~CustomCameraOffsetsModule() {
    if (m_clientUpdateSubscription != 0) {
        bedrocktools::events::bus().unsubscribe(
            m_clientUpdateSubscription);

        m_clientUpdateSubscription = 0;
    }

    if (g_cameraMod == this) {
        g_cameraMod = nullptr;
    }

    g_isSolidBlockingBlock = nullptr;
}

bool CustomCameraOffsetsModule::isThirdPerson() const {
    return m_thirdPerson;
}

void CustomCameraOffsetsModule::onInit() {
    if (!g_isSolidBlockingBlock) {
        const uintptr_t address =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::
                    BlockSourceIsSolidBlockingBlock);

        if (address != 0) {
            g_isSolidBlockingBlock =
                reinterpret_cast<
                    BlockSourceIsSolidBlockingBlockFn>(address);

            m_isSolidBlockingBlockTarget =
                reinterpret_cast<void*>(address);
        }
    }

    if (m_clientUpdateSubscription == 0) {
        m_clientUpdateSubscription =
            bedrocktools::events::bus().subscribe<
                bedrocktools::events::ClientInstanceUpdateEvent>(
                [](bedrocktools::events::ClientInstanceUpdateEvent& event) {

                    if (!g_cameraMod ||
                        !g_cameraMod->enabled ||
                        !event.clientInstance) {
                        return;
                    }

                    g_cameraMod->applyCamera(
                        event.clientInstance);
                },
                bedrocktools::events::EventPriority::Last);
    }
}

void CustomCameraOffsetsModule::onEnable() {
    m_hasLastCamera = false;
}

void CustomCameraOffsetsModule::onDisable() {
    m_hasLastCamera = false;
    m_thirdPerson = false;
}

bedrocktools::sdk::Vec3
CustomCameraOffsetsModule::collisionSafeCamera(
    bedrocktools::sdk::BlockSource* region,
    const bedrocktools::sdk::Vec3& origin,
    const bedrocktools::sdk::Vec3& target) const {

    if (!region || !g_isSolidBlockingBlock) {
        return target;
    }

    const bedrocktools::sdk::Vec3 delta =
        sub(target, origin);

    const float distance = length(delta);

    if (distance <= 0.001f) {
        return target;
    }

    // Limit the amount of work for unusually large custom offsets.
    const float testedDistance =
        std::min(distance, 32.0f);

    const bedrocktools::sdk::Vec3 direction =
        normalize(delta);

    // Use a small conservative step so the camera cannot skip a
    // thin solid block between the player and the requested camera.
    constexpr float step = 0.10f;

    float lastSafeDistance = 0.0f;

    for (float d = step; d <= testedDistance; d += step) {
        const bedrocktools::sdk::Vec3 point =
            add(origin, mul(direction, d));

        // Test the camera point and a small vertical pair around it.
        // This makes the obstruction check less permissive around
        // block edges without attempting to replace Minecraft's
        // full player collision system.
        const float samples[] = {
            point.y,
            point.y + 0.20f,
            point.y - 0.20f
        };

        bool blocked = false;

        for (float sampleY : samples) {
            BlockPosRaw blockPos{
                static_cast<int32_t>(
                    std::floor(point.x)),
                static_cast<int32_t>(
                    std::floor(sampleY)),
                static_cast<int32_t>(
                    std::floor(point.z))
            };

            if (g_isSolidBlockingBlock(
                    region,
                    &blockPos)) {

                blocked = true;
                break;
            }
        }

        if (blocked) {
            // Stop slightly before the obstruction so the camera
            // does not sit on the block boundary.
            return add(
                origin,
                mul(
                    direction,
                    std::max(
                        0.0f,
                        lastSafeDistance - 0.05f)));
        }

        lastSafeDistance = d;
    }

    return add(
        origin,
        mul(direction, testedDistance));
}

void CustomCameraOffsetsModule::applyCamera(
    bedrocktools::sdk::ClientInstance* client) {

    auto* player = client->localPlayer();

    auto* renderer =
        client->levelRenderer();

    if (!player || !renderer) {
        m_thirdPerson = false;
        m_hasLastCamera = false;
        return;
    }

    auto* rendererPlayer =
        renderer->playerRenderer();

    auto* region =
        client->region();

    if (!rendererPlayer || !region) {
        m_thirdPerson = false;
        m_hasLastCamera = false;
        return;
    }

    // Vanilla has already updated this value because this callback
    // runs after ClientInstance::update().
    const bedrocktools::sdk::Vec3 vanillaCamera =
        rendererPlayer->cameraPosition();

    const bedrocktools::sdk::Vec3 playerPos =
        player->position();

    // Detect the actual vanilla third-person state from the camera
    // distance rather than changing Minecraft's perspective state.
    const bedrocktools::sdk::Vec3 cameraDelta =
        sub(vanillaCamera, playerPos);

    m_thirdPerson =
        length(cameraDelta) > 1.0f;

    if (m_onlyThirdPerson && !m_thirdPerson) {
        m_hasLastCamera = false;
        return;
    }

    const auto rotation =
        player->rotation();

    const bedrocktools::sdk::Vec3 requestedCamera =
        buildOffset(
            playerPos,
            rotation,
            *this);

    // Collision is resolved BEFORE smoothing. This prevents smoothing
    // from slowly moving the camera through an obstruction.
    const bedrocktools::sdk::Vec3 safeCamera =
        collisionSafeCamera(
            region,
            {
                playerPos.x,
                playerPos.y + 1.62f,
                playerPos.z
            },
            requestedCamera);

    bedrocktools::sdk::Vec3 finalCamera =
        safeCamera;

    if (m_hasLastCamera) {
        finalCamera =
            lerpVec(
                m_lastCamera,
                safeCamera,
                m_smoothness);
    }

    m_lastCamera =
        finalCamera;

    m_hasLastCamera = true;

    rendererPlayer->cameraPosition() =
        finalCamera;
}

void CustomCameraOffsetsModule::loadConfig(
    const nlohmann::json& j) {

    Module::loadConfig(j);

    if (j.contains("m_offsetX"))
        m_offsetX =
            j["m_offsetX"].get<float>();

    if (j.contains("m_offsetY"))
        m_offsetY =
            j["m_offsetY"].get<float>();

    if (j.contains("m_offsetZ"))
        m_offsetZ =
            j["m_offsetZ"].get<float>();

    if (j.contains("m_yawOffset"))
        m_yawOffset =
            j["m_yawOffset"].get<float>();

    if (j.contains("m_pitchOffset"))
        m_pitchOffset =
            j["m_pitchOffset"].get<float>();

    if (j.contains("m_smoothness"))
        m_smoothness =
            j["m_smoothness"].get<float>();

    if (j.contains("m_onlyThirdPerson"))
        m_onlyThirdPerson =
            j["m_onlyThirdPerson"].get<bool>();

    if (j.contains("m_useLookDirection"))
        m_useLookDirection =
            j["m_useLookDirection"].get<bool>();

    m_smoothness =
        std::clamp(
            m_smoothness,
            0.0f,
            1.0f);
}

void CustomCameraOffsetsModule::saveConfig(
    nlohmann::json& j) {

    Module::saveConfig(j);

    j["m_offsetX"] = m_offsetX;
    j["m_offsetY"] = m_offsetY;
    j["m_offsetZ"] = m_offsetZ;
    j["m_yawOffset"] = m_yawOffset;
    j["m_pitchOffset"] = m_pitchOffset;
    j["m_smoothness"] = m_smoothness;
    j["m_onlyThirdPerson"] =
        m_onlyThirdPerson;
    j["m_useLookDirection"] =
        m_useLookDirection;
}
