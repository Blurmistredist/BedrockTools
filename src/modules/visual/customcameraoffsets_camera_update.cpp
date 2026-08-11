#include "customcameraoffsets.hpp"

#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cmath>

static CustomCameraOffsetsModule* g_cameraMod = nullptr;

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
        (180.0f + rotation.y + mod.m_yawOffset) * (PI / 180.0f);

    const float pitch =
        (-(rotation.x + mod.m_pitchOffset)) * (PI / 180.0f);

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
            mod.m_offsetY * 0.0f +
            forward.x * mod.m_offsetZ,

        playerPos.y +
            mod.m_offsetY +
            forward.y * mod.m_offsetZ,

        playerPos.z +
            right.z * mod.m_offsetX +
            forward.z * mod.m_offsetZ
    };
}

static bool detectThirdPerson(
    const bedrocktools::sdk::Vec3& camera,
    const bedrocktools::sdk::Vec3& playerPos) {

    // Actor::position() is the player's world/body position. In first
    // person the camera is near eye height above it; in third person the
    // camera is several blocks away. Comparing against an eye-height
    // position avoids confusing normal first-person height with distance.
    constexpr float eyeHeight = 1.62f;

    const float dx = camera.x - playerPos.x;
    const float dy = camera.y - (playerPos.y + eyeHeight);
    const float dz = camera.z - playerPos.z;

    const float distanceSq = dx * dx + dy * dy + dz * dz;

    // Vanilla third-person is normally several blocks from the player.
    // 1.0 block gives enough margin for first-person camera movement.
    return distanceSq > 1.0f;
}

CustomCameraOffsetsModule::CustomCameraOffsetsModule()
    : Module(
        "Custom Camera Offsets",
        "Moves the third-person camera with configurable offsets.") {

    g_cameraMod = this;
}

CustomCameraOffsetsModule::~CustomCameraOffsetsModule() {
    if (m_clientUpdateSubscription != 0) {
        bedrocktools::events::bus().unsubscribe(m_clientUpdateSubscription);
        m_clientUpdateSubscription = 0;
    }

    if (g_cameraMod == this) {
        g_cameraMod = nullptr;
    }
}

bool CustomCameraOffsetsModule::isThirdPerson() const {
    return m_thirdPerson;
}

void CustomCameraOffsetsModule::onInit() {
    if (m_clientUpdateSubscription != 0) {
        return;
    }

    m_clientUpdateSubscription =
        bedrocktools::events::bus().subscribe<
            bedrocktools::events::ClientInstanceUpdateEvent>(
            [](bedrocktools::events::ClientInstanceUpdateEvent& event) {

                if (!g_cameraMod ||
                    !g_cameraMod->enabled ||
                    !event.clientInstance) {
                    return;
                }

                g_cameraMod->applyCamera(event.clientInstance);
            },
            bedrocktools::events::EventPriority::Last);
}

void CustomCameraOffsetsModule::onEnable() {
    m_hasLastCamera = false;
}

void CustomCameraOffsetsModule::onDisable() {
    m_hasLastCamera = false;
}

void CustomCameraOffsetsModule::applyCamera(
    bedrocktools::sdk::ClientInstance* client) {

    auto* player = client->localPlayer();
    auto* renderer = client->levelRenderer();

    if (!player || !renderer) {
        m_thirdPerson = false;
        m_hasLastCamera = false;
        return;
    }

    auto* rendererPlayer = renderer->playerRenderer();

    if (!rendererPlayer) {
        m_thirdPerson = false;
        m_hasLastCamera = false;
        return;
    }

    // This event fires after ClientInstance::update() has completed.
    // That is deliberate: vanilla has already calculated mCamPos, so
    // our value is not immediately overwritten by the camera update.
    const bedrocktools::sdk::Vec3 vanillaCamera =
        rendererPlayer->cameraPosition();

    const bedrocktools::sdk::Vec3 playerPos =
        player->position();

    m_thirdPerson =
        detectThirdPerson(vanillaCamera, playerPos);

    if (m_onlyThirdPerson && !m_thirdPerson) {
        m_hasLastCamera = false;
        return;
    }

    const bedrocktools::sdk::Vec2 rotation =
        player->rotation();

    bedrocktools::sdk::Vec3 target =
        buildOffset(playerPos, rotation, *this);

    if (m_hasLastCamera) {
        target = lerpVec(
            m_lastCamera,
            target,
            m_smoothness);
    }

    m_lastCamera = target;
    m_hasLastCamera = true;

    rendererPlayer->cameraPosition() = target;
}

void CustomCameraOffsetsModule::loadConfig(
    const nlohmann::json& j) {

    Module::loadConfig(j);

    if (j.contains("m_offsetX"))
        m_offsetX = j["m_offsetX"].get<float>();

    if (j.contains("m_offsetY"))
        m_offsetY = j["m_offsetY"].get<float>();

    if (j.contains("m_offsetZ"))
        m_offsetZ = j["m_offsetZ"].get<float>();

    if (j.contains("m_yawOffset"))
        m_yawOffset = j["m_yawOffset"].get<float>();

    if (j.contains("m_pitchOffset"))
        m_pitchOffset = j["m_pitchOffset"].get<float>();

    if (j.contains("m_smoothness"))
        m_smoothness = j["m_smoothness"].get<float>();

    if (j.contains("m_onlyThirdPerson"))
        m_onlyThirdPerson =
            j["m_onlyThirdPerson"].get<bool>();

    if (j.contains("m_useLookDirection"))
        m_useLookDirection =
            j["m_useLookDirection"].get<bool>();

    m_smoothness =
        std::clamp(m_smoothness, 0.0f, 1.0f);
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
    j["m_onlyThirdPerson"] = m_onlyThirdPerson;
    j["m_useLookDirection"] = m_useLookDirection;
}
