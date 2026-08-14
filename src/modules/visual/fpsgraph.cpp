#include "fpsgraph.hpp"

#include "core/Runtime.hpp"
#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {
FPSGraphModule* g_fps = nullptr;

void (*g_pingOriginal)(void*) = nullptr;

void pingHook(void* self) {
    if (g_pingOriginal) g_pingOriginal(self);
    if (!g_fps || !g_fps->enabled || !self) return;

    const int ping = *reinterpret_cast<int*>(
        reinterpret_cast<std::uintptr_t>(self) +
        bedrocktools::sdk::offsets::RakNetConnector::mAvgPing);

    if (ping >= 0 && ping < 100000)
        g_fps->setPingFromHook(ping);
}

float clampFinite(float value, float lo, float hi) {
    if (!std::isfinite(value)) return lo;
    return std::clamp(value, lo, hi);
}

std::uint32_t rgba(std::uint32_t rgb, float alpha = 1.0f) {
    const auto a = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (a << 24) | (rgb & 0x00FFFFFFu);
}

float readProcessRamMb() {
    std::ifstream statm("/proc/self/statm");
    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) return 0.0f;

    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return 0.0f;
    return static_cast<float>(
        (static_cast<double>(residentPages) * static_cast<double>(pageSize)) /
        (1024.0 * 1024.0));
}

void addRect(std::vector<PLModMenu_DrawCommand>& cmds,
             float x, float y, float w, float h, std::uint32_t color) {
    PLModMenu_DrawCommand c{};
    c.type = PL_DRAW_RECT_FILLED;
    c.x = x; c.y = y; c.w = w; c.h = h; c.color = color;
    cmds.push_back(c);
}

void addLine(std::vector<PLModMenu_DrawCommand>& cmds,
             float x, float y, float w, float h, float thickness,
             std::uint32_t color) {
    PLModMenu_DrawCommand c{};
    c.type = PL_DRAW_LINE;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.size = thickness; c.color = color;
    cmds.push_back(c);
}

void addText(std::vector<PLModMenu_DrawCommand>& cmds,
             std::vector<std::string>& strings,
             float x, float y, float size, std::uint32_t color,
             const std::string& text) {
    strings.push_back(text);
    PLModMenu_DrawCommand c{};
    c.type = PL_DRAW_TEXT;
    c.x = x; c.y = y;
    c.color = color;
    c.size = size;
    c.fontId = "minecraft";
    c.text = strings.back().c_str();
    cmds.push_back(c);
}
}

FPSGraphModule::FPSGraphModule()
    : Module("FPS Graph", "A performance monitor with FPS, jitter, RAM, ping, 1% low, MSPT and TPS.") {
    g_fps = this;
    isHudModule = true;
    hudPosX = 60.0f;
    hudPosY = 60.0f;
}

FPSGraphModule::~FPSGraphModule() {
    if (g_fps == this) g_fps = nullptr;
}

void FPSGraphModule::setPingFromHook(int ping) {
    m_pingMs = ping;
}

void FPSGraphModule::onInit() {
    // Register the bundled Minecraft font if available. Failure is harmless.
    static bool fontRegistered = false;
    if (!fontRegistered) {
        const auto path = bedrocktools::core::Runtime::get().resourceDirectory() / "minecraft.ttf";
        std::ifstream file(path, std::ios::binary);
        if (file) {
            std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
            if (!data.empty()) {
                pl::modmenu::registerFont("minecraft", data);
                fontRegistered = true;
            }
        }
    }

    if (!m_pingHooked) {
        const auto addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RaknetUpdate);
        if (addr) {
            m_pingPatchTarget = reinterpret_cast<void*>(addr);
            m_pingHooked = static_cast<bool>(bedrocktools::hooks::install(
                m_pingPatchTarget,
                reinterpret_cast<void*>(pingHook),
                reinterpret_cast<void**>(&g_pingOriginal)));
        }
    }

    if (!m_tickSubscription) {
        m_tickSubscription =
            bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
                [this](auto&) {
                    if (!enabled) return;
                    const auto now = std::chrono::steady_clock::now();
                    if (m_lastTick.time_since_epoch().count() != 0) {
                        const double dt =
                            std::chrono::duration<double, std::milli>(now - m_lastTick).count();
                        if (dt > 1.0 && dt < 1000.0) {
                            m_mspt = static_cast<float>(dt);
                            m_tps = static_cast<float>(1000.0 / dt);
                            m_tps = std::clamp(m_tps, 0.0f, 20.0f);

                            m_msptHistory.push_back(m_mspt);
                            m_tpsHistory.push_back(m_tps);
                            while (m_msptHistory.size() > 240) m_msptHistory.pop_front();
                            while (m_tpsHistory.size() > 240) m_tpsHistory.pop_front();
                        }
                    }
                    m_lastTick = now;
                });
    }
}

void FPSGraphModule::onEnable() {
    m_lastFrame = std::chrono::steady_clock::now();
    m_lastTick = {};
    m_haveFrameTime = false;
    m_frameAccumulator = 0.0;
}

void FPSGraphModule::onDisable() {
}

void FPSGraphModule::updateFrameMetrics(double dt) {
    if (dt <= 0.0001 || dt > 1.0) return;

    const float frameMs = static_cast<float>(dt * 1000.0);
    const float fps = static_cast<float>(1.0 / dt);
    m_fps = std::clamp(fps, 0.0f, 10000.0f);

    m_frameTimeHistory.push_back(frameMs);
    while (m_frameTimeHistory.size() > 600) m_frameTimeHistory.pop_front();

    m_fpsHistory.push_back(m_fps);
    const size_t maxSamples = static_cast<size_t>(
        std::clamp(m_historySeconds * 120.0f, 60.0f, 600.0f));
    while (m_fpsHistory.size() > maxSamples) m_fpsHistory.pop_front();

    const double mean = std::accumulate(m_fpsHistory.begin(), m_fpsHistory.end(), 0.0) /
                        static_cast<double>(m_fpsHistory.size());
    m_averageFps = static_cast<float>(mean);

    double frameMean = std::accumulate(
        m_frameTimeHistory.begin(), m_frameTimeHistory.end(), 0.0);
    frameMean /= static_cast<double>(std::max<size_t>(1, m_frameTimeHistory.size()));

    double variance = 0.0;
    for (float sample : m_frameTimeHistory) {
        const double d = static_cast<double>(sample) - frameMean;
        variance += d * d;
    }
    variance /= static_cast<double>(std::max<size_t>(1, m_frameTimeHistory.size()));
    m_jitterMs = static_cast<float>(std::sqrt(std::max(0.0, variance)));

    // 1% low = average of the slowest 1% of the sampled FPS values.
    std::vector<float> sorted(m_fpsHistory.begin(), m_fpsHistory.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t count = std::max<size_t>(1, sorted.size() / 100);
    m_onePercentLow =
        std::accumulate(sorted.begin(), sorted.begin() + count, 0.0f) /
        static_cast<float>(count);

    m_ramMb = readProcessRamMb();
    m_frameAccumulator += dt;
}

void FPSGraphModule::onFrame() {
    if (!enabled) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_lastFrame.time_since_epoch().count() == 0) {
        m_lastFrame = now;
        return;
    }

    const double dt = std::chrono::duration<double>(now - m_lastFrame).count();
    m_lastFrame = now;
    updateFrameMetrics(dt);

    if (m_superPerformanceModeThing) {
        const double t = std::chrono::duration<double>(now.time_since_epoch()).count();
        m_fps = 2000.0f + static_cast<float>((std::sin(t * 2.7) + 1.0) * 500.0);
    }

    if (m_displayMode == "Numbers Only")
        drawNumbersOnly();
    else
        drawGraph();
}

void FPSGraphModule::drawGraph() {
    constexpr float panelW = 540.0f;
    constexpr float panelH = 560.0f;
    const float scale = std::clamp(m_scale, 0.5f, 2.0f);
    const float x = m_posX;
    const float y = m_posY;
    const float w = panelW * scale;
    const float h = panelH * scale;

    std::vector<PLModMenu_DrawCommand> cmds;
    std::vector<std::string> strings;

    auto addText = [&](float tx, float ty, float size, uint32_t color, const std::string& text) {
        PLModMenu_DrawCommand c{};
        c.type = PL_DRAW_TEXT;
        c.x = tx; c.y = ty; c.w = 0.f; c.h = 1.f;
        c.size = size; c.color = color; c.text = text.c_str();
        strings.push_back(text);
        c.text = strings.back().c_str();
        cmds.push_back(c);
    };

    auto addRect = [&](float rx, float ry, float rw, float rh, uint32_t color) {
        PLModMenu_DrawCommand c{};
        c.type = PL_DRAW_RECT_FILLED;
        c.x = rx; c.y = ry; c.w = rw; c.h = rh; c.color = color;
        cmds.push_back(c);
    };

    auto addLine = [&](float lx, float ly, float lw, float lh, float thickness, uint32_t color) {
        PLModMenu_DrawCommand c{};
        c.type = PL_DRAW_LINE;
        c.x = lx; c.y = ly; c.w = lw; c.h = lh;
        c.size = thickness; c.color = color;
        cmds.push_back(c);
    };

    // Fixed layout: every metric has a dedicated column, and every row has
    // fixed vertical spacing. Nothing shares a text column.
    addRect(x, y, w, h, 0xEE10161Cu);
    addRect(x + 3.f * scale, y + 3.f * scale, w - 6.f * scale, h - 6.f * scale, 0xE9161D25u);

    const float left = x + 22.f * scale;
    const float right = x + w - 22.f * scale;
    const float labelCol = left;
    const float valueCol = x + w - 170.f * scale;
    const float unitCol = x + w - 78.f * scale;
    const float titleSize = 17.f * scale;
    const float labelSize = 24.f * scale;
    const float valueSize = 28.f * scale;
    const float unitSize = 16.f * scale;

    addText(left, y + 12.f * scale, titleSize, 0xFFB7C5D5u, "FPS PERFORMANCE");

    // Header metrics.
    const float headerY = y + 42.f * scale;
    addText(left, headerY, labelSize, 0xFFE7EEF6u, "FPS");
    addText(x + 150.f * scale, headerY, 36.f * scale, 0xFF2BEA76u,
            std::to_string(static_cast<int>(std::round(m_fps))));
    addText(x + 285.f * scale, headerY, labelSize, 0xFFE7EEF6u, "AVG");
    addText(x + 355.f * scale, headerY, 36.f * scale, 0xFF59C7FFu,
            std::to_string(static_cast<int>(std::round(m_averageFps))));

    // FPS history.
    float graphY = y + 88.f * scale;
    const float graphH = 92.f * scale;
    addText(left, graphY - 18.f * scale, titleSize, 0xFF93A7BBu, "FPS HISTORY");
    addRect(left, graphY, right - left, graphH, 0xFF102B37u);

    const size_t nFps = std::min<size_t>(m_fpsHistory.size(), 120);
    if (nFps) {
        const float bw = (right - left) / static_cast<float>(nFps);
        const float maxFps = std::max(120.f, m_averageFps * 1.4f);
        for (size_t i = 0; i < nFps; ++i) {
            const float v = std::clamp(m_fpsHistory[m_fpsHistory.size() - nFps + i] / maxFps, 0.f, 1.f);
            addRect(left + i * bw,
                    graphY + graphH * (1.f - v),
                    std::max(1.f, bw - 1.f),
                    graphH * v,
                    0xFF27BFEA);
        }
    }

    float rowY = graphY + graphH + 18.f * scale;

    // A stable two-column metric table.
    addText(labelCol, rowY, titleSize, 0xFF93A7BBu, "CURRENT METRICS");
    rowY += 24.f * scale;

    const float col2 = x + w * 0.54f;
    const float value2 = x + w - 170.f * scale;
    const float unit2 = x + w - 78.f * scale;
    const float metricRow = 36.f * scale;

    auto metric = [&](float lx, float vx, float ux, float ly,
                      const char* label, const std::string& value,
                      const char* unit, uint32_t valueColor) {
        addText(lx, ly, labelSize, 0xFFDCE6F0u, label);
        addText(vx, ly, valueSize, valueColor, value);
        addText(ux, ly + 4.f * scale, unitSize, 0xFFE3AA63u, unit);
    };

    if (m_showJitter || m_showRam) {
        if (m_showJitter)
            metric(labelCol, valueCol, unitCol, rowY, "Jitter",
                   std::to_string(static_cast<int>(std::round(m_jitterMs))), "ms", 0xFFB98BFFu);
        if (m_showRam)
            metric(col2, value2, unit2, rowY, "RAM",
                   std::to_string(static_cast<int>(std::round(m_ramMb))), "MB", 0xFF8ED6FFu);
        rowY += metricRow;
    }

    if (m_showPing || m_showOnePercentLow) {
        if (m_showPing)
            metric(labelCol, valueCol, unitCol, rowY, "Ping",
                   std::to_string(m_pingMs), "ms", 0xFFFFD05Cu);
        if (m_showOnePercentLow)
            metric(col2, value2, unit2, rowY, "1% Low",
                   std::to_string(static_cast<int>(std::round(m_onePercentLow))), "fps", 0xFFFF9B73u);
        rowY += metricRow;
    }

    if (m_showMspt || m_showTps) {
        char msptBuf[32], tpsBuf[32];
        std::snprintf(msptBuf, sizeof(msptBuf), "%.1f", m_mspt);
        std::snprintf(tpsBuf, sizeof(tpsBuf), "%.1f", m_tps);
        if (m_showMspt)
            metric(labelCol, valueCol, unitCol, rowY, "MSPT", msptBuf, "ms", 0xFFE0A52Fu);
        if (m_showTps)
            metric(col2, value2, unit2, rowY, "TPS", tpsBuf, "tps", 0xFF5ED17Eu);
        rowY += metricRow;
    }

    addLine(left, rowY + 4.f * scale, right - left, 1.f, 1.5f * scale, 0xFF425362u);
    rowY += 18.f * scale;

    // Tick-time graph.
    if (m_showMspt) {
        addText(left, rowY, titleSize, 0xFF93A7BBu, "MSPT HISTORY");
        rowY += 18.f * scale;
        const float mh = 54.f * scale;
        addRect(left, rowY, right - left, mh, 0xFF3A2B16u);

        const size_t n = std::min<size_t>(m_msptHistory.size(), 120);
        if (n) {
            const float bw = (right - left) / static_cast<float>(n);
            for (size_t i = 0; i < n; ++i) {
                const float v = std::clamp(m_msptHistory[m_msptHistory.size() - n + i] / 50.f, 0.f, 1.f);
                addRect(left + i * bw, rowY + mh * (1.f - v),
                        std::max(1.f, bw - 1.f), mh * v, 0xFFE1A33A);
            }
        }
        rowY += mh + 12.f * scale;
    }

    if (m_superPerformanceModeThing) {
        addText(left, y + h - 24.f * scale, 15.f * scale, 0xFFFFC45Bu,
                "SUPER PERFORMANCE MODE THING");
    }

    submitDrawCommands(moduleId, cmds);
}


void FPSGraphModule::drawNumbersOnly() {
    constexpr float panelW = 440.0f;
    constexpr float panelH = 360.0f;
    const float scale = std::clamp(m_scale, 0.5f, 2.0f);
    const float x = m_posX;
    const float y = m_posY;
    const float w = panelW * scale;
    const float h = panelH * scale;

    std::vector<PLModMenu_DrawCommand> cmds;
    std::vector<std::string> strings;
    addRect(cmds, x, y, w, h, 0xEE11171Du);

    const float labelX = x + 20.0f * scale;
    const float valueX = x + w - 150.0f * scale;
    const float unitX = x + w - 70.0f * scale;
    const float font = 25.0f * scale;
    const float valueFont = 28.0f * scale;
    const float unitFont = 18.0f * scale;

    struct Row { const char* label; std::string value; const char* unit; bool show; };
    const Row rows[] = {
        {"FPS", std::to_string(static_cast<int>(std::round(m_fps))), "fps", m_showFps},
        {"Avg", std::to_string(static_cast<int>(std::round(m_averageFps))), "fps", m_showAverage},
        {"Jitter", std::to_string(static_cast<int>(std::round(m_jitterMs))), "ms", m_showJitter},
        {"RAM", std::to_string(static_cast<int>(std::round(m_ramMb))), "mb", m_showRam},
        {"Ping", std::to_string(m_pingMs), "ms", m_showPing},
        {"1%Low", std::to_string(static_cast<int>(std::round(m_onePercentLow))), "fps", m_showOnePercentLow},
        {"MSPT", ([&]{ char b[32]; std::snprintf(b, sizeof(b), "%.1f", m_mspt); return std::string(b); })(), "ms", m_showMspt},
        {"TPS", ([&]{ char b[32]; std::snprintf(b, sizeof(b), "%.1f", m_tps); return std::string(b); })(), "tps", m_showTps}
    };

    float cy = y + 38.0f * scale;
    for (const auto& row : rows) {
        if (!row.show) continue;
        addText(cmds, strings, labelX, cy, font, 0xFFD9E3EEu, row.label);
        addText(cmds, strings, valueX, cy, valueFont, 0xFF27E86Bu, row.value);
        addText(cmds, strings, unitX, cy + 2.0f * scale, unitFont, 0xFFE2AA61u, row.unit);
        cy += 39.0f * scale;
    }

    if (m_superPerformanceModeThing) {
        addText(cmds, strings, labelX, cy + 5.0f * scale,
                16.0f * scale, 0xFFE2AA61u, "SUPER PERFORMANCE MODE THING");
    }

    submitDrawCommands(moduleId, cmds);
}

void FPSGraphModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_scale = j.value("m_scale", m_scale);
    m_posX = j.value("m_posX", m_posX);
    m_posY = j.value("m_posY", m_posY);
    m_historySeconds = j.value("m_historySeconds", m_historySeconds);
    m_displayMode = j.value("m_displayMode", m_displayMode);
    if (m_displayMode.empty()) m_displayMode = "Graph";

    m_showFps = j.value("m_showFps", m_showFps);
    m_showAverage = j.value("m_showAverage", m_showAverage);
    m_showJitter = j.value("m_showJitter", m_showJitter);
    m_showRam = j.value("m_showRam", m_showRam);
    m_showPing = j.value("m_showPing", m_showPing);
    m_showOnePercentLow = j.value("m_showOnePercentLow", m_showOnePercentLow);
    m_showMspt = j.value("m_showMspt", m_showMspt);
    m_showTps = j.value("m_showTps", m_showTps);
    m_superPerformanceModeThing =
        j.value("m_superPerformanceModeThing", m_superPerformanceModeThing);
}

void FPSGraphModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_scale"] = m_scale;
    j["m_posX"] = m_posX;
    j["m_posY"] = m_posY;
    j["m_historySeconds"] = m_historySeconds;
    j["m_displayMode"] = m_displayMode;

    j["m_showFps"] = m_showFps;
    j["m_showAverage"] = m_showAverage;
    j["m_showJitter"] = m_showJitter;
    j["m_showRam"] = m_showRam;
    j["m_showPing"] = m_showPing;
    j["m_showOnePercentLow"] = m_showOnePercentLow;
    j["m_showMspt"] = m_showMspt;
    j["m_showTps"] = m_showTps;
    j["m_superPerformanceModeThing"] = m_superPerformanceModeThing;
}
