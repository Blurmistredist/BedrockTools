#pragma once

#include "../Module.hpp"

#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>
#include <nlohmann/json.hpp>

class CustomCameraOffsetsModule final : public Module {
public:
    CustomCameraOffsetsModule();
    ~CustomCameraOffsetsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool isThirdPerson() const;

    // Dynamic third-person camera
    float m_distance = 4.0f;
    float m_minDistance = 0.75f;
    float m_maxDistance = 10.0f;

    float m_shoulderX = 0.0f;
    float m_height = 1.55f;

    // Additional orbit offsets. The camera still follows the
    // player's live look direction, so the mobile aim direction
    // remains the source of truth for interaction.
    float m_yawOffset = 0.0f;
    float m_pitchOffset = 0.0f;

    float m_positionSmoothing = 0.30f;

    bool m_onlyThirdPerson = true;
    bool m_dynamicDistance = true;

    // Always show a center reticle while this module is enabled.
    bool m_forceCrosshair = true;
    float m_crosshairSize = 7.0f;
    float m_crosshairThickness = 2.0f;
    float m_crosshairGap = 3.0f;

    bool isThirdPersonActive() const {
        return m_thirdPerson;
    }

    void setThirdPersonState(bool value) {
        m_thirdPerson = value;
    }

private:
    bool m_thirdPerson = false;
    bool m_hasLastCamera = false;
    bool m_renderHooked = false;
    bool m_perspectiveHooked = false;
    bool m_cursorHooked = false;

    void* m_renderTarget = nullptr;
    void* m_perspectiveTarget = nullptr;
    void* m_cursorTarget = nullptr;

    bedrocktools::sdk::Vec3 m_lastCamera{
        0.0f, 0.0f, 0.0f
    };

    void updateCamera();
    void drawCrosshair();

    bedrocktools::sdk::Vec3 calculateCameraPosition(
        const bedrocktools::sdk::Vec3& playerPos,
        const bedrocktools::sdk::Vec2& playerRotation,
        bedrocktools::sdk::BlockSource* region) const;

    bedrocktools::sdk::Vec3 resolveCameraCollision(
        const bedrocktools::sdk::Vec3& origin,
        const bedrocktools::sdk::Vec3& target,
        bedrocktools::sdk::BlockSource* region) const;
};
