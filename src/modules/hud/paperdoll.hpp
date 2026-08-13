#pragma once

#include "../Module.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

class PaperDollModule : public Module {
public:
    PaperDollModule();
    ~PaperDollModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onLocalPlayerTick(void* localPlayer);

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float hudPosX = 40.f;
    float hudPosY = 80.f;
    float scale = 1.5f;
    bool background = false;
    float backgroundOpacity = 0.35f;
    bool showHeadLayer = true;

private:
    static constexpr int TEX_W = 64;
    static constexpr int TEX_H = 96;
    using Pixels = std::array<std::uint8_t, TEX_W * TEX_H * 4>;

    std::mutex m_mutex;
    std::string m_imageId = "paperdoll_local";
    bool m_hasImage = false;
    int m_refreshTicks = 0;

    void updateFromPlayer(void* player);
};
