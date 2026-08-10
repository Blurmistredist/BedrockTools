#include "fpsgraph.hpp"
#include "modules/ModuleRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {
static FPSGraphModule* g_fpsGraphMod = nullptr;

float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ') width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.8f;
        else width += size * 0.58f;
    }
    return width;
}

uint32_t applyAlpha(uint32_t color, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return (static_cast<uint32_t>(alpha * 255.0f) << 24) | (color & 0x00FFFFFF);
}
}

FPSGraphModule::FPSGraphModule()
    : Module("FPS Graph", "Shows a live FPS history graph on screen.") {
    g_fpsGraphMod = this;
}

FPSGraphModule::~FPSGraphModule() {
    if (g_fpsGraphMod == this) g_fpsGraphMod = nullptr;
}

void FPSGraphModule::onEnable() {
    m_lastFrame = Clock::now();
    m_hasLastFrame = false;
    m_history.clear();
    m_currentFps = 0.0f;
    m_averageFps = 0.0f;
    m_peakFps = 0.0f;
}

void FPSGraphModule::onDisable() {
    m_hasLastFrame = false;
}

void FPSGraphModule::onFrame() {
    if (!enabled) return;

    const auto now = Clock::now();
    if (m_hasLastFrame) {
        const std::chrono::duration<float> dt = now - m_lastFrame;
        const float seconds = dt.count();
        if (seconds > 0.000001f) {
            m_currentFps = 1.0f / seconds;
            m_history.push_back(m_currentFps);
            if (static_cast<int>(m_history.size()) > m_historySize) {
                m_history.erase(m_history.begin(), m_history.begin() + (m_history.size() - static_cast<std::size_t>(m_historySize)));
            }

            float sum = 0.0f;
            m_peakFps = 0.0f;
            for (float fps : m_history) {
                sum += fps;
                m_peakFps = std::max(m_peakFps, fps);
            }
            m_averageFps = m_history.empty() ? 0.0f : (sum / static_cast<float>(m_history.size()));
        }
    } else {
        m_hasLastFrame = true;
    }
    m_lastFrame = now;

    std::vector<PLModMenu_DrawCommand> cmds;

    const float pad = 6.0f;
    const float graphX = hudPosX;
    const float graphY = hudPosY;
    const float graphW = std::max(70.0f, m_width);
    const float graphH = std::max(36.0f, m_height);
    const float innerX = graphX + pad;
    const float innerY = graphY + pad + (m_showStats ? (m_size + 4.0f) : 0.0f);
    const float innerW = graphW - pad * 2.0f;
    const float innerH = graphH - pad * 2.0f - (m_showStats ? (m_size + 4.0f) : 0.0f);

    if (m_background) {
        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = graphX;
        bgCmd.y = graphY;
        bgCmd.w = graphW;
        bgCmd.h = graphH;
        bgCmd.color = applyAlpha(0x000000, m_backgroundOpacity);
        cmds.push_back(bgCmd);
    }

    const float step = innerW / static_cast<float>(std::max(1, m_historySize));
    const float barW = std::max(1.0f, step * 0.85f);
    const std::size_t startIndex = m_history.size() > static_cast<std::size_t>(m_historySize)
        ? m_history.size() - static_cast<std::size_t>(m_historySize)
        : 0;

    if (m_showGrid) {
        const int lines = 4;
        for (int i = 1; i < lines; ++i) {
            PLModMenu_DrawCommand line = {};
            line.type = PL_DRAW_LINE;
            line.x = innerX;
            line.y = innerY + (innerH / lines) * i;
            line.w = innerW;
            line.h = 0.0f;
            line.size = 1.0f;
            line.color = 0x3AFFFFFF;
            cmds.push_back(line);
        }
    }

    for (std::size_t i = startIndex; i < m_history.size(); ++i) {
        const float fps = m_history[i];
        const float normalized = std::clamp(fps / std::max(1.0f, m_scaleFps), 0.0f, 1.0f);
        const float barH = std::max(1.0f, innerH * normalized);
        const float x = innerX + static_cast<float>(i - startIndex) * step;
        const float y = innerY + (innerH - barH);

        uint32_t color = 0xFF3CD23C;
        if (fps < 30.0f) color = 0xFFFF4D4D;
        else if (fps < 60.0f) color = 0xFFFFC64D;
        else if (fps < 90.0f) color = 0xFF5AC8FA;

        PLModMenu_DrawCommand barCmd = {};
        barCmd.type = PL_DRAW_RECT_FILLED;
        barCmd.x = x;
        barCmd.y = y;
        barCmd.w = barW;
        barCmd.h = barH;
        barCmd.color = color;
        cmds.push_back(barCmd);
    }

    if (m_showStats) {
        char buf[128];
        snprintf(buf, sizeof(buf), "FPS %.0f  AVG %.0f  MAX %.0f", m_currentFps, m_averageFps, m_peakFps);

        PLModMenu_DrawCommand txtCmd = {};
        txtCmd.type = PL_DRAW_TEXT;
        txtCmd.x = graphX + pad;
        txtCmd.y = graphY + 2.0f;
        txtCmd.w = calcTextWidth(buf, m_size) + 8.0f;
        txtCmd.h = m_size + 2.0f;
        txtCmd.color = 0xFFFFFFFF;
        txtCmd.size = m_size;
        txtCmd.text = buf;
        cmds.push_back(txtCmd);
    }

    submitDrawCommands(moduleId, cmds);
}

void FPSGraphModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_width")) m_width = j["m_width"].get<float>();
    if (j.contains("m_height")) m_height = j["m_height"].get<float>();
    if (j.contains("m_size")) m_size = j["m_size"].get<float>();
    if (j.contains("m_historySize")) m_historySize = j["m_historySize"].get<int>();
    if (j.contains("m_scaleFps")) m_scaleFps = j["m_scaleFps"].get<float>();
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_showStats")) m_showStats = j["m_showStats"].get<bool>();
    if (j.contains("m_showGrid")) m_showGrid = j["m_showGrid"].get<bool>();
    m_historySize = std::max(10, m_historySize);
    m_scaleFps = std::max(1.0f, m_scaleFps);
}

void FPSGraphModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_width"] = m_width;
    j["m_height"] = m_height;
    j["m_size"] = m_size;
    j["m_historySize"] = m_historySize;
    j["m_scaleFps"] = m_scaleFps;
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_showStats"] = m_showStats;
    j["m_showGrid"] = m_showGrid;
}
