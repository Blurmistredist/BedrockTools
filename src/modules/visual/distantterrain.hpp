#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

class DistantTerrainModule final : public Module {
public:
    DistantTerrainModule();
    ~DistantTerrainModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float m_closeDistance = 32.0f;
    float m_farDistance = 96.0f;
    float m_fartherDistance = 256.0f;

    int m_closeResolution = 4;
    int m_farResolution = 16;
    int m_fartherResolution = 48;

    int m_samplesPerFrame = 6;
    int m_maxCachedCells = 12000;

    float m_heightRange = 192.0f;
    float m_surfaceOffset = 0.08f;
    float m_transitionWidth = 8.0f;
    float m_opacity = 0.70f;

    bool m_smoothLodTransition = true;
    bool m_showWater = true;
    bool m_showVegetationMass = true;

    bool m_rebuildButton = false;

    void renderLods(void* levelRenderer, void* screenContext);

private:
    struct CellKey {
        int lod;
        int x;
        int z;
        bool operator==(const CellKey& other) const noexcept {
            return lod == other.lod && x == other.x && z == other.z;
        }
    };

    struct CellKeyHash {
        std::size_t operator()(const CellKey& k) const noexcept {
            std::size_t h = static_cast<std::size_t>(k.lod + 17);
            h = h * 0x9E3779B185EBCA87ULL + static_cast<std::uint32_t>(k.x);
            h = h * 0x9E3779B185EBCA87ULL + static_cast<std::uint32_t>(k.z);
            return h;
        }
    };

    struct Cell {
        float height = 0.0f;
        float slope = 0.0f;
        std::uint8_t material = 0;
        bool valid = false;
        std::uint64_t stamp = 0;
    };

    std::unordered_map<CellKey, Cell, CellKeyHash> m_cells;
    std::vector<CellKey> m_pending;
    std::size_t m_pendingCursor = 0;
    std::uint64_t m_stamp = 1;
    bool m_hasPlayer = false;
    bedrocktools::sdk::Vec3 m_playerPosition{0.0f, 0.0f, 0.0f};

    bedrocktools::events::Subscription m_tickSubscription = 0;

    void* m_renderTarget = nullptr;
    void* m_tessBeginTarget = nullptr;
    void* m_tessColorTarget = nullptr;
    void* m_tessVertexTarget = nullptr;
    void* m_renderMeshTarget = nullptr;
    void* m_renderMaterialTarget = nullptr;

    bool m_renderHooked = false;

    void rebuildPending();
    void processSamples();
    bool sampleCell(const CellKey& key, int resolution, bedrocktools::sdk::BlockSource* region);
    bool findSurfaceHeight(int x, int z, bedrocktools::sdk::BlockSource* region, float& height, std::uint8_t& material) const;
    void clearCache();
};
