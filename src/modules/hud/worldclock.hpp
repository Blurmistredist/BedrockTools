#pragma once

#include "../Module.hpp"

class WorldClockModule : public Module {
public:
    WorldClockModule();
    ~WorldClockModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void updateWorldTicks(int ticks) { m_worldTicks = ticks; }
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    int m_worldTicks = 0;

    float hudPosX = 20.0f;
    float hudPosY = 20.0f;
    bool isHudModule = true;

    float m_size = 40.0f;
    bool m_background = false;
    float m_backgroundOpacity = 0.5f;
};
