#pragma once

#include "../Module.hpp"

class ChunkFadeModule : public Module {
public:
    ChunkFadeModule();
    ~ChunkFadeModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float m_fadeStart = 64.0f;
    float m_fadeEnd = 160.0f;
    float m_fadeOpacity = 0.20f;

    // Kept for config compatibility with older versions.
    // Chunk Fade no longer writes these values into the renderer,
    // because doing so creates a visible sky/fog horizon seam.
    float m_fadeColorR = 0.80f;
    float m_fadeColorG = 0.84f;
    float m_fadeColorB = 0.92f;

    bool m_onlyThirdPerson = false;

    bool isThirdPerson() const;

    void setThirdPersonState(bool value) {
        m_thirdPerson = value;
    }

    bool isThirdPersonActive() const {
        return m_thirdPerson;
    }

private:
    bool m_patched = false;
    bool m_thirdPerson = false;

    void* m_patchTarget = nullptr;
    void* m_perspectiveTarget = nullptr;
};
