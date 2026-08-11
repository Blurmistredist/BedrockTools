#pragma once

#include "../Module.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <nlohmann/json.hpp>

class CustomCameraOffsetsModule : public Module {
public:
    CustomCameraOffsetsModule();
    ~CustomCameraOffsetsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool isThirdPerson() const;

    float m_offsetX = 0.0f;
    float m_offsetY = 1.55f;
    float m_offsetZ = -4.0f;

    float m_yawOffset = 0.0f;
    float m_pitchOffset = 0.0f;

    float m_smoothness = 0.35f;

    bool m_onlyThirdPerson = true;
    bool m_useLookDirection = true;

    bool isThirdPersonActive() const {
        return m_thirdPerson;
    }

private:
    bool m_thirdPerson = false;
    bool m_hasLastCamera = false;

    bedrocktools::sdk::Vec3 m_lastCamera{0.0f, 0.0f, 0.0f};

    bedrocktools::events::Subscription m_clientUpdateSubscription = 0;

    void applyCamera(bedrocktools::sdk::ClientInstance* client);
};
