#pragma once

#include "../Module.hpp"

class VideoSettingsModule : public Module {
public:
    VideoSettingsModule();
    ~VideoSettingsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

public:
    bool m_worldRenderingTest = false;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    // This is intentionally a hard test switch: when enabled, the
    // LevelRenderer world pass is skipped while the surrounding UI can
    // continue to render normally.
    void applyPatch();
};
