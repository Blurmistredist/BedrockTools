#pragma once

#include "../Module.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <chrono>
#include <deque>

class FPSGraphModule final : public Module {
public:
    FPSGraphModule();
    ~FPSGraphModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void setPingFromHook(int ping);

private:
    float m_scale = 1.0f;
    float m_posX = 60.0f;
    float m_posY = 60.0f;
    float m_historySeconds = 4.0f;
    std::string m_displayMode = "Graph";

    bool m_showFps = true;
    bool m_showAverage = true;
    bool m_showJitter = true;
    bool m_showRam = true;
    bool m_showPing = true;
    bool m_showOnePercentLow = true;
    bool m_showMspt = true;
    bool m_showTps = true;
    bool m_superPerformanceModeThing = false;

    float m_fps = 0.0f;
    float m_averageFps = 0.0f;
    float m_jitterMs = 0.0f;
    float m_ramMb = 0.0f;
    int m_pingMs = 0;
    float m_onePercentLow = 0.0f;
    float m_mspt = 0.0f;
    float m_tps = 0.0f;

    std::deque<float> m_fpsHistory;
    std::deque<float> m_frameTimeHistory;
    std::deque<float> m_msptHistory;
    std::deque<float> m_tpsHistory;

    bool m_haveFrameTime = false;
    std::chrono::steady_clock::time_point m_lastFrame{};
    std::chrono::steady_clock::time_point m_lastTick{};

    bedrocktools::events::Subscription m_tickSubscription = 0;
    void* m_pingPatchTarget = nullptr;
    bool m_pingHooked = false;

    void updateFrameMetrics(double dt);
    void drawGraph();
    void drawNumbersOnly();
};
