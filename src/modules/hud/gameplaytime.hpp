#pragma once

#include "../Module.hpp"

#include <chrono>
#include <string>

class GameplayTimeModule : public Module {
public:
    GameplayTimeModule();
    ~GameplayTimeModule() override = default;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(void* player);
    void onClientUpdate(void* clientInstance);

    float hudPosX = 20.0f;
    float hudPosY = 150.0f;
    bool isHudModule = true;

    float textSize = 40.0f;
    bool background = true;
    float backgroundOpacity = 0.55f;
    bool showLabel = false;
    bool showSeconds = true;

private:
    using Clock = std::chrono::steady_clock;

    bool m_inSession = false;
    Clock::time_point m_sessionStart{};
    int m_missingPlayerUpdates = 0;

    std::chrono::steady_clock::duration elapsed() const;
    static std::string formatDuration(std::chrono::steady_clock::duration duration);
};
