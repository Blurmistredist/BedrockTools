#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>

class CustomCameraOffsetsModule : public Module {
public:
    CustomCameraOffsetsModule();
    ~CustomCameraOffsetsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float m_offsetX = 0.0f;
    float m_offsetY = 1.55f;
    float m_offsetZ = -4.0f;
    float m_yawOffset = 0.0f;
    float m_pitchOffset = 0.0f;
    float m_smoothness = 0.35f;
    bool  m_onlyThirdPerson = true;
    bool  m_useLookDirection = true;

    bool isThirdPerson() const;

private:
    bool m_patched = false;
    bool m_thirdPerson = false;

    void* m_patchTarget = nullptr;
    void* m_perspectiveTarget = nullptr;

    bedrocktools::sdk::Vec3 m_lastCamera{0.0f, 0.0f, 0.0f};
    bool m_hasLastCamera = false;

public:
    void setThirdPersonState(bool value) {
        m_thirdPerson = value;
    }

    bool isThirdPersonActive() const {
        return m_thirdPerson;
    }

    bool hasLastCamera() const {
        return m_hasLastCamera;
    }

    void setLastCamera(const bedrocktools::sdk::Vec3& camera) {
        m_lastCamera = camera;
        m_hasLastCamera = true;
    }

    const bedrocktools::sdk::Vec3& lastCamera() const {
        return m_lastCamera;
    }
};
