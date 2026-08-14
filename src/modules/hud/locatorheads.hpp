#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

class LocatorHeadsModule : public Module {
public:
    LocatorHeadsModule();
    ~LocatorHeadsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(void* localPlayer);

    float hudPosX = 400.f;
    float hudPosY = 55.f;
    bool isHudModule = true;

    float m_barWidth = 520.f;
    float m_barHeight = 32.f;
    float m_markerSize = 20.f;
    float m_range = 60.f;          // half-angle in degrees; vanilla-style total FOV = 120°
    float m_maxDistance = 128.f;
    float m_opacity = 1.0f;
    float m_backgroundOpacity = 0.45f;
    bool m_showNames = false;
    bool m_showDistance = false;
    bool m_showVerticalArrows = true;
    bool m_fadeWithDistance = true;
    bool m_showCardinals = true;

private:
    struct PlayerMarker {
        void* actor = nullptr;
        std::string name;
        std::string imageId;
    };

    std::mutex m_mutex;
    std::vector<PlayerMarker> m_players;
    void* m_localPlayer = nullptr;
    float m_localYaw = 0.f;
    int m_refreshTicks = 20;
    std::unordered_map<void*, std::string> m_imageCache;

    void refreshPlayers(void* localPlayer);
};
