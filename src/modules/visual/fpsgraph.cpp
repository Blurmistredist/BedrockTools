#include "fpsgraph.hpp"
#include "modules/ModuleRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
static FPSGraphModule* g_fpsGraphMod = nullptr;

float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ')
            width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
            width += size * 0.8f;
        else
            width += size * 0.58f;
    }
    return width;
}

uint32_t applyAlpha(uint32_t color, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return (static_cast<uint32_t>(alpha * 255.0f) << 24) |
           (color & 0x00FFFFFF);
}
}

FPSGraphModule::FPSGraphModule()
    : Module("FPS Graph", "Shows a live FPS history graph on screen.") {
    g_fpsGraphMod = this;
}

FPSGraphModule::~FPSGraphModule() {
    if (g_fpsGraphMod == this)
        g_fpsGraphMod = nullptr;
}

void FPSGraphModule::onEnable() {
    m_lastFrame = Clock::now();
    m_fakeStart = Clock::now();
    m_hasLastFrame = false;
    m_history.clear();
    m_currentFps = 0.0f;
    m_averageFps = 0.0f;
    m_peakFps = 0.0f;
}

void FPSGraphModule::onDisable() {
    m_hasLastFrame = false;
    m_history.clear();
}

float FPSGraphModule::getDisplayedFps(float realFps) const {
    if (!m_superPerformanceModeThing)
        return realFps;

    /*
     * Cosmetic moving value, deliberately constrained to 2000-3000.
     *
     * Multiple sine waves give it a smooth, non-repeating feel instead
     * of jumping randomly every frame.
     */
    const auto now = Clock::now();
    const float t =
        std::chrono::duration<float>(now - m_fakeStart).count();

    const float wave1 = std::sin(t * 1.17f);
    const float wave2 = std::sin(t * 2.31f + 1.7f);
    const float wave3 = std::sin(t * 0.43f + 0.8f);

    const float normalized =
        std::clamp(
            0.50f +
            wave1 * 0.25f +
            wave2 * 0.15f +
            wave3 * 0.10f,
            0.0f,
            1.0f);

    return 2000.0f + normalized * 1000.0f;
}

void FPSGraphModule::rebuildStatistics() {
    if (m_history.empty()) {
        m_averageFps = 0.0f;
        m_peakFps = 0.0f;
        return;
    }

    float sum = 0.0f;
    float peak = 0.0f;

    for (const float fps : m_history) {
        sum += fps;
        peak = std::max(peak, fps);
    }

    m_averageFps =
        sum / static_cast<float>(m_history.size());

    m_peakFps = peak;
}

void FPSGraphModule::onFrame() {
    if (!enabled)
        return;

    const auto now = Clock::now();

    if (m_hasLastFrame) {
        const std::chrono::duration<float> dt =
            now - m_lastFrame;

        const float seconds = dt.count();

        if (seconds > 0.000001f) {
            const float realFps = 1.0f / seconds;
            const float displayedFps =
                getDisplayedFps(realFps);

            m_currentFps = displayedFps;

            m_history.push_back(displayedFps);

            if (static_cast<int>(m_history.size()) > m_historySize) {
                m_history.erase(
                    m_history.begin(),
                    m_history.begin() +
                        (m_history.size() -
                         static_cast<std::size_t>(m_historySize)));
            }

            rebuildStatistics();
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

    /*
     * Numbers-only mode intentionally skips all graph/background/grid
     * rendering. This makes the compact mode actually compact.
     */
    if (m_numbersOnly) {
        const float line = m_size + 3.0f;
        const float textX = graphX + pad;
        float textY = graphY + 2.0f;

        char buf[128];

        std::snprintf(
            buf,
            sizeof(buf),
            "FPS %.0f",
            m_currentFps);

        PLModMenu_DrawCommand fpsCmd = {};
        fpsCmd.type = PL_DRAW_TEXT;
        fpsCmd.x = textX;
        fpsCmd.y = textY;
        fpsCmd.w = calcTextWidth(buf, m_size) + 8.0f;
        fpsCmd.h = m_size + 2.0f;
        fpsCmd.color = 0xFFFFFFFF;
        fpsCmd.size = m_size;
        fpsCmd.text = buf;
        cmds.push_back(fpsCmd);

        textY += line;

        std::snprintf(
            buf,
            sizeof(buf),
            "Avg %.0f",
            m_averageFps);

        PLModMenu_DrawCommand avgCmd = {};
        avgCmd.type = PL_DRAW_TEXT;
        avgCmd.x = textX;
        avgCmd.y = textY;
        avgCmd.w = calcTextWidth(buf, m_size) + 8.0f;
        avgCmd.h = m_size + 2.0f;
        avgCmd.color = 0xFFFFFFFF;
        avgCmd.size = m_size;
        avgCmd.text = buf;
        cmds.push_back(avgCmd);

        textY += line;

        std::snprintf(
            buf,
            sizeof(buf),
            "Max %.0f",
            m_peakFps);

        PLModMenu_DrawCommand maxCmd = {};
        maxCmd.type = PL_DRAW_TEXT;
        maxCmd.x = textX;
        maxCmd.y = textY;
        maxCmd.w = calcTextWidth(buf, m_size) + 8.0f;
        maxCmd.h = m_size + 2.0f;
        maxCmd.color = 0xFFFFFFFF;
        maxCmd.size = m_size;
        maxCmd.text = buf;
        cmds.push_back(maxCmd);

        submitDrawCommands(moduleId, cmds);
        return;
    }

    const float innerX = graphX + pad;
    const float innerY =
        graphY + pad +
        (m_showStats ? (m_size + 4.0f) : 0.0f);

    const float innerW =
        graphW - pad * 2.0f;

    const float innerH =
        graphH -
        pad * 2.0f -
        (m_showStats ? (m_size + 4.0f) : 0.0f);

    if (m_background) {
        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = graphX;
        bgCmd.y = graphY;
        bgCmd.w = graphW;
        bgCmd.h = graphH;
        bgCmd.color =
            applyAlpha(
                0x000000,
                m_backgroundOpacity);
        cmds.push_back(bgCmd);
    }

    const float step =
        innerW /
        static_cast<float>(
            std::max(1, m_historySize));

    const float barW =
        std::max(1.0f, step * 0.85f);

    const std::size_t startIndex =
        m_history.size() >
            static_cast<std::size_t>(m_historySize)
            ? m_history.size() -
                  static_cast<std::size_t>(m_historySize)
            : 0;

    if (m_showGrid) {
        constexpr int lines = 4;

        for (int i = 1; i < lines; ++i) {
            PLModMenu_DrawCommand line = {};
            line.type = PL_DRAW_LINE;
            line.x = innerX;
            line.y =
                innerY +
                (innerH / lines) * i;
            line.w = innerW;
            line.h = 0.0f;
            line.size = 1.0f;
            line.color = 0x3AFFFFFF;
            cmds.push_back(line);
        }
    }

    for (std::size_t i = startIndex;
         i < m_history.size();
         ++i) {

        const float fps =
            m_history[i];

        const float normalized =
            std::clamp(
                fps /
                    std::max(
                        1.0f,
                        m_scaleFps),
                0.0f,
                1.0f);

        const float barH =
            std::max(
                1.0f,
                innerH * normalized);

        const float x =
            innerX +
            static_cast<float>(
                i - startIndex) *
                step;

        const float y =
            innerY +
            (innerH - barH);

        uint32_t color =
            0xFF3CD23C;

        if (!m_superPerformanceModeThing) {
            if (fps < 30.0f)
                color = 0xFFFF4D4D;
            else if (fps < 60.0f)
                color = 0xFFFFC64D;
            else if (fps < 90.0f)
                color = 0xFF5AC8FA;
        } else {
            // Keep the easter-egg graph consistently "super".
            color = 0xFF3CFF78;
        }

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

        std::snprintf(
            buf,
            sizeof(buf),
            "FPS %.0f  AVG %.0f  MAX %.0f",
            m_currentFps,
            m_averageFps,
            m_peakFps);

        PLModMenu_DrawCommand txtCmd = {};
        txtCmd.type = PL_DRAW_TEXT;
        txtCmd.x = graphX + pad;
        txtCmd.y = graphY + 2.0f;
        txtCmd.w =
            calcTextWidth(buf, m_size) + 8.0f;
        txtCmd.h = m_size + 2.0f;
        txtCmd.color = 0xFFFFFFFF;
        txtCmd.size = m_size;
        txtCmd.text = buf;
        cmds.push_back(txtCmd);
    }

    submitDrawCommands(
        moduleId,
        cmds);
}

void FPSGraphModule::loadConfig(
    const nlohmann::json& j) {

    Module::loadConfig(j);

    if (j.contains("hudPosX"))
        hudPosX = j["hudPosX"].get<float>();

    if (j.contains("hudPosY"))
        hudPosY = j["hudPosY"].get<float>();

    if (j.contains("isHudModule"))
        isHudModule =
            j["isHudModule"].get<bool>();

    if (j.contains("m_width"))
        m_width =
            j["m_width"].get<float>();

    if (j.contains("m_height"))
        m_height =
            j["m_height"].get<float>();

    if (j.contains("m_size"))
        m_size =
            j["m_size"].get<float>();

    if (j.contains("m_historySize"))
        m_historySize =
            j["m_historySize"].get<int>();

    if (j.contains("m_scaleFps"))
        m_scaleFps =
            j["m_scaleFps"].get<float>();

    if (j.contains("m_background"))
        m_background =
            j["m_background"].get<bool>();

    if (j.contains("m_backgroundOpacity"))
        m_backgroundOpacity =
            j["m_backgroundOpacity"].get<float>();

    if (j.contains("m_showStats"))
        m_showStats =
            j["m_showStats"].get<bool>();

    if (j.contains("m_showGrid"))
        m_showGrid =
            j["m_showGrid"].get<bool>();

    if (j.contains("m_numbersOnly"))
        m_numbersOnly =
            j["m_numbersOnly"].get<bool>();

    if (j.contains("m_superPerformanceModeThing"))
        m_superPerformanceModeThing =
            j["m_superPerformanceModeThing"].get<bool>();

    m_historySize =
        std::max(10, m_historySize);

    m_scaleFps =
        std::max(1.0f, m_scaleFps);

    m_backgroundOpacity =
        std::clamp(
            m_backgroundOpacity,
            0.0f,
            1.0f);

    m_size =
        std::max(1.0f, m_size);
}

void FPSGraphModule::saveConfig(
    nlohmann::json& j) {

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
    j["m_backgroundOpacity"] =
        m_backgroundOpacity;
    j["m_showStats"] = m_showStats;
    j["m_showGrid"] = m_showGrid;

    j["m_numbersOnly"] =
        m_numbersOnly;

    // Keep this as the final FPS-specific config entry.
    // ModuleMenu.cpp needs the small ordering patch below to guarantee
    // it is rendered as the final setting.
    j["m_superPerformanceModeThing"] =
        m_superPerformanceModeThing;
}
