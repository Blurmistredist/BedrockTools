#pragma once

#include "../Module.hpp"
#include <chrono>
#include <vector>

class FPSGraphModule : public Module {
public:
    FPSGraphModule();
    ~FPSGraphModule() override;

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float hudPosX = 24.0f;
    float hudPosY = 24.0f;
    bool isHudModule = true;

    float m_width = 220.0f;
    float m_height = 88.0f;
    float m_size = 14.0f;
    int   m_historySize = 120;
    float m_scaleFps = 144.0f;
    bool  m_background = true;
    float m_backgroundOpacity = 0.55f;
    bool  m_showStats = true;
    bool  m_showGrid = true;

    // When enabled, only the numerical statistics are drawn.
    bool  m_numbersOnly = false;

    // Cosmetic easter egg: replaces displayed FPS values with a
    // continuously moving 2000-3000 FPS value. It never changes
    // the actual game frame rate or timing.
    bool  m_superPerformanceModeThing = false;

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_lastFrame{};
    Clock::time_point m_fakeStart{};
    bool m_hasLastFrame = false;
    std::vector<float> m_history;
    float m_currentFps = 0.0f;
    float m_averageFps = 0.0f;
    float m_peakFps = 0.0f;

    float getDisplayedFps(float realFps) const;
    void rebuildStatistics();
};
