#pragma once

#include "../Module.hpp"

class ChunkFadeModule final : public Module {
public:
    ChunkFadeModule();
    ~ChunkFadeModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float fadeStart() const { return m_fadeStart; }
    float fadeEnd() const { return m_fadeEnd; }
    float fadeOpacity() const { return m_fadeOpacity; }
    bool onlyThirdPerson() const { return m_onlyThirdPerson; }

private:
    void* m_patchTarget = nullptr;
    bool m_hooked = false;

    // Offsets are relative to the fog values produced by Minecraft.
    float m_fadeStart = 0.0f;
    float m_fadeEnd = 100.0f;
    float m_fadeOpacity = 1.0f;
    bool m_onlyThirdPerson = false;
};
