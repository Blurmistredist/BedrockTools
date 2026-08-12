#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>

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

    bool forceThirdPerson() const { return m_forceThirdPerson; }
    bool collision() const { return m_collision; }
    float distance() const { return m_distance; }
    float sideOffset() const { return m_sideOffset; }
    float verticalOffset() const { return m_verticalOffset; }
    float smoothness() const { return m_smoothness; }
    bool dynamic() const { return m_dynamic; }

    bedrocktools::sdk::Vec3& smoothedCamera() { return m_smoothedCamera; }
    bool hasSmoothedCamera() const { return m_haveSmoothedCamera; }
    void setHasSmoothedCamera(bool value) { m_haveSmoothedCamera = value; }

public:
    bedrocktools::sdk::Vec3 calculateCameraPosition(
        const bedrocktools::sdk::Vec3& playerPos,
        const bedrocktools::sdk::Vec2& rotation,
        void* region) const;

    bedrocktools::sdk::Vec3 resolveCollision(
        const bedrocktools::sdk::Vec3& start,
        const bedrocktools::sdk::Vec3& desired,
        void* region) const;

private:
    void drawCrosshair();

    void* m_renderPatchTarget = nullptr;
    bool m_renderHooked = false;
    bool m_perspectiveHooked = false;

    bool m_forceThirdPerson = true;
    bool m_collision = true;
    bool m_crosshair = true;
    bool m_dynamic = true;

    float m_distance = 4.0f;
    float m_sideOffset = 0.65f;
    float m_verticalOffset = 0.35f;
    float m_smoothness = 14.0f;
    float m_crosshairSize = 7.0f;
    float m_crosshairThickness = 2.0f;

    bedrocktools::sdk::Vec3 m_smoothedCamera{0.0f, 0.0f, 0.0f};
    bool m_haveSmoothedCamera = false;
};
