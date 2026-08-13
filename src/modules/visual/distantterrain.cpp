#include "distantterrain.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/BlockSource.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {

using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshFn = void (*)(void*, void*, void*, char*);
using BlockSourceGetBlockFn = void* (*)(void*, const bedrocktools::sdk::BlockPos*, int32_t);
using BlockSourceSolidFn = bool (*)(void*, const bedrocktools::sdk::BlockPos*);

struct HashedString {
    std::uint64_t hash = 0;
    std::string str;
    const HashedString* last = nullptr;
    explicit HashedString(const char* value) : str(value ? value : ""), last(nullptr) {
        constexpr std::uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t prime = 0x100000001B3ULL;
        hash = offset;
        for (char c : str) hash = static_cast<std::uint64_t>(static_cast<unsigned char>(c)) ^ (prime * hash);
    }
};

struct MaterialPtr { void* shared[2]; };

static DistantTerrainModule* g_mod = nullptr;
static TessellatorBeginFn s_begin = nullptr;
static TessellatorColorFn s_color = nullptr;
static TessellatorVertexFn s_vertex = nullptr;
static RenderMeshFn s_renderMesh = nullptr;
static BlockSourceGetBlockFn s_getBlock = nullptr;
static BlockSourceSolidFn s_isSolid = nullptr;
static std::uintptr_t s_materialGroup = 0;
static MaterialPtr* s_material = nullptr;
static void (*s_renderOrig)(void*, void*, void*) = nullptr;

static std::uintptr_t resolveADRP(std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            const std::uintptr_t page = (reinterpret_cast<std::uintptr_t>(&insns[i]) & ~0xFFFULL)
                + ((static_cast<std::int64_t>(static_cast<std::uint64_t>(((insn >> 3) & 0x1FFFFC) | ((insn >> 29) & 3)) << 43)) >> 31);
            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
    }
    return 0;
}

static MaterialPtr* getMaterial(const char* name) {
    if (!s_materialGroup) return nullptr;
    auto** vtable = *reinterpret_cast<void***>(s_materialGroup);
    if (!vtable || !vtable[2]) return nullptr;
    using GetMaterialFn = MaterialPtr* (*)(void*, const HashedString*);
    HashedString hs(name);
    return reinterpret_cast<GetMaterialFn>(vtable[2])(reinterpret_cast<void*>(s_materialGroup), &hs);
}

static void emitQuad(void* tessellator, float x0, float y0, float z0,
                     float x1, float y1, float z1,
                     float x2, float y2, float z2,
                     float x3, float y3, float z3,
                     float camX, float camY, float camZ) {
    s_vertex(tessellator, x0 - camX, y0 - camY, z0 - camZ);
    s_vertex(tessellator, x1 - camX, y1 - camY, z1 - camZ);
    s_vertex(tessellator, x2 - camX, y2 - camY, z2 - camZ);
    s_vertex(tessellator, x3 - camX, y3 - camY, z3 - camZ);
    s_vertex(tessellator, x3 - camX, y3 - camY, z3 - camZ);
    s_vertex(tessellator, x2 - camX, y2 - camY, z2 - camZ);
    s_vertex(tessellator, x1 - camX, y1 - camY, z1 - camZ);
    s_vertex(tessellator, x0 - camX, y0 - camY, z0 - camZ);
}

static float fadeFactor(float distance, float start, float end, float width) {
    if (distance <= start) return 0.0f;
    if (distance >= end) return 1.0f;
    const float t = (distance - start) / std::max(0.001f, end - start);
    if (width <= 0.0f) return t;
    const float smooth = t * t * (3.0f - 2.0f * t);
    return std::clamp(smooth, 0.0f, 1.0f);
}

static void renderHook(void* self, void* screenContext, void* a3) {
    if (s_renderOrig) s_renderOrig(self, screenContext, a3);
    if (!g_mod || !g_mod->enabled || !screenContext) return;
    g_mod->renderLods(self, screenContext);
}

} // namespace

DistantTerrainModule::DistantTerrainModule()
    : Module("Distant Terrain", "Client-side multi-resolution distant terrain renderer.") {
    g_mod = this;
}

DistantTerrainModule::~DistantTerrainModule() {
    if (m_tickSubscription) {
        bedrocktools::events::bus().unsubscribe(m_tickSubscription);
        m_tickSubscription = 0;
    }
    if (g_mod == this) g_mod = nullptr;
}

void DistantTerrainModule::clearCache() {
    m_cells.clear();
    m_pending.clear();
    m_pendingCursor = 0;
    ++m_stamp;
}

void DistantTerrainModule::rebuildPending() {
    m_pending.clear();
    m_pendingCursor = 0;
    if (!m_hasPlayer) return;

    const int px = static_cast<int>(std::floor(m_playerPosition.x));
    const int pz = static_cast<int>(std::floor(m_playerPosition.z));

    auto appendRing = [&](int lod, float distance, int resolution) {
        const int maxCells = static_cast<int>(std::ceil(distance / std::max(1, resolution)));
        const int cx = static_cast<int>(std::floor(static_cast<float>(px) / resolution));
        const int cz = static_cast<int>(std::floor(static_cast<float>(pz) / resolution));
        for (int dz = -maxCells; dz <= maxCells; ++dz) {
            for (int dx = -maxCells; dx <= maxCells; ++dx) {
                if (dx == 0 && dz == 0 && lod > 0) continue;
                const float worldX = (cx + dx + 0.5f) * resolution;
                const float worldZ = (cz + dz + 0.5f) * resolution;
                const float d = std::sqrt((worldX - px) * (worldX - px) + (worldZ - pz) * (worldZ - pz));
                if (d > distance + resolution * 1.5f) continue;
                m_pending.push_back(CellKey{lod, cx + dx, cz + dz});
            }
        }
    };

    appendRing(0, m_closeDistance, m_closeResolution);
    appendRing(1, m_farDistance, m_farResolution);
    appendRing(2, m_fartherDistance, m_fartherResolution);

    std::sort(m_pending.begin(), m_pending.end(), [&](const CellKey& a, const CellKey& b) {
        const float ra = (a.lod == 0 ? m_closeResolution : a.lod == 1 ? m_farResolution : m_fartherResolution);
        const float rb = (b.lod == 0 ? m_closeResolution : b.lod == 1 ? m_farResolution : m_fartherResolution);
        const float ax = (a.x + 0.5f) * ra - m_playerPosition.x;
        const float az = (a.z + 0.5f) * ra - m_playerPosition.z;
        const float bx = (b.x + 0.5f) * rb - m_playerPosition.x;
        const float bz = (b.z + 0.5f) * rb - m_playerPosition.z;
        return ax * ax + az * az < bx * bx + bz * bz;
    });
}

bool DistantTerrainModule::findSurfaceHeight(int x, int z, bedrocktools::sdk::BlockSource* region,
                                              float& height, std::uint8_t& material) const {
    if (!region || !s_getBlock || !s_isSolid) return false;

    const int top = static_cast<int>(std::floor(m_playerPosition.y + m_heightRange));
    const int bottom = static_cast<int>(std::floor(m_playerPosition.y - m_heightRange));
    const int coarseStep = 6;
    int lastAirY = top;

    for (int y = top; y >= bottom; y -= coarseStep) {
        const bedrocktools::sdk::BlockPos pos{x, y, z};
        if (s_isSolid(region, &pos)) {
            int lo = std::max(bottom, y - coarseStep);
            int hi = y;
            while (hi - lo > 1) {
                const int mid = (lo + hi) / 2;
                const bedrocktools::sdk::BlockPos p{x, mid, z};
                if (s_isSolid(region, &p)) hi = mid;
                else lo = mid;
            }
            height = static_cast<float>(hi + 1);
            material = 0;
            const bedrocktools::sdk::BlockPos topPos{x, hi, z};
            if (s_getBlock(region, &topPos, 0) != nullptr) material = 1;
            return true;
        }
        lastAirY = y;
        (void)lastAirY;
    }
    return false;
}

bool DistantTerrainModule::sampleCell(const CellKey& key, int resolution,
                                      bedrocktools::sdk::BlockSource* region) {
    const int x = key.x * resolution + resolution / 2;
    const int z = key.z * resolution + resolution / 2;

    float centerHeight = 0.0f;
    std::uint8_t material = 0;
    if (!findSurfaceHeight(x, z, region, centerHeight, material)) {
        m_cells[key] = Cell{};
        return false;
    }

    float slope = 0.0f;
    float neighbor = centerHeight;
    float h = 0.0f;
    std::uint8_t ignored = 0;
    if (findSurfaceHeight(x + resolution, z, region, h, ignored)) slope = std::max(slope, std::fabs(h - centerHeight));
    if (findSurfaceHeight(x - resolution, z, region, h, ignored)) slope = std::max(slope, std::fabs(h - centerHeight));
    if (findSurfaceHeight(x, z + resolution, region, h, ignored)) slope = std::max(slope, std::fabs(h - centerHeight));
    if (findSurfaceHeight(x, z - resolution, region, h, ignored)) slope = std::max(slope, std::fabs(h - centerHeight));
    (void)neighbor;

    Cell& cell = m_cells[key];
    cell.height = centerHeight;
    cell.slope = std::min(1.0f, slope / std::max(1.0f, static_cast<float>(resolution)));
    cell.material = material;
    cell.valid = true;
    cell.stamp = m_stamp++;

    if (static_cast<int>(m_cells.size()) > m_maxCachedCells) {
        auto oldest = m_cells.begin();
        for (auto it = m_cells.begin(); it != m_cells.end(); ++it) {
            if (it->second.stamp < oldest->second.stamp) oldest = it;
        }
        if (oldest != m_cells.end()) m_cells.erase(oldest);
    }
    return true;
}

void DistantTerrainModule::processSamples() {
    if (!enabled || !m_hasPlayer) return;
    auto* client = bedrocktools::sdk::ClientInstance::current();
    if (!client) return;
    auto* region = client->region();
    if (!region) return;
    if (m_pending.empty() || m_pendingCursor >= m_pending.size()) rebuildPending();
    if (m_pending.empty()) return;

    int done = 0;
    while (done < std::max(1, m_samplesPerFrame) && m_pendingCursor < m_pending.size()) {
        const CellKey key = m_pending[m_pendingCursor++];
        const int resolution = key.lod == 0 ? m_closeResolution : key.lod == 1 ? m_farResolution : m_fartherResolution;
        sampleCell(key, resolution, region);
        ++done;
    }
}

void DistantTerrainModule::onInit() {
    const auto render = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (render) m_renderTarget = reinterpret_cast<void*>(render);

    const auto begin = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (begin) { m_tessBeginTarget = reinterpret_cast<void*>(begin); s_begin = reinterpret_cast<TessellatorBeginFn>(begin); }
    const auto color = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (color) { m_tessColorTarget = reinterpret_cast<void*>(color); s_color = reinterpret_cast<TessellatorColorFn>(color); }
    const auto vertex = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (vertex) { m_tessVertexTarget = reinterpret_cast<void*>(vertex); s_vertex = reinterpret_cast<TessellatorVertexFn>(vertex); }

    auto mesh = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!mesh) mesh = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    if (mesh) { m_renderMeshTarget = reinterpret_cast<void*>(mesh); s_renderMesh = reinterpret_cast<RenderMeshFn>(mesh); }

    const auto getBlock = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (getBlock) s_getBlock = reinterpret_cast<BlockSourceGetBlockFn>(getBlock);
    const auto solid = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (solid) s_isSolid = reinterpret_cast<BlockSourceSolidFn>(solid);

    const auto material = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (material) {
        const auto group = resolveADRP(reinterpret_cast<std::uint32_t*>(material), 96, 2);
        if (group) s_materialGroup = group + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
    }

    if (!m_tickSubscription) {
        m_tickSubscription = bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
            [](bedrocktools::events::LocalPlayerTickEvent& event) {
                if (!g_mod || !event.player) return;
                g_mod->m_playerPosition = event.player->position();
                g_mod->m_hasPlayer = true;
                if (g_mod->enabled && (g_mod->m_pending.empty() || g_mod->m_pendingCursor >= g_mod->m_pending.size())) {
                    g_mod->rebuildPending();
                }
            });
    }
}

void DistantTerrainModule::onEnable() {
    clearCache();
    rebuildPending();
    if (m_renderTarget && !m_renderHooked) {
        bedrocktools::hooks::install(m_renderTarget, reinterpret_cast<void*>(renderHook), reinterpret_cast<void**>(&s_renderOrig));
        m_renderHooked = true;
    }
}

void DistantTerrainModule::onDisable() {
    clearCache();
}

void DistantTerrainModule::onFrame() {
    if (!enabled) return;
    processSamples();
    if (m_rebuildButton) {
        m_rebuildButton = false;
        clearCache();
        rebuildPending();
    }
}

void DistantTerrainModule::renderLods(void* levelRenderer, void* screenContext) {
    if (!s_begin || !s_color || !s_vertex || !s_renderMesh || !screenContext || !m_hasPlayer) return;
    if (!s_material) {
        s_material = getMaterial("selection_box");
        if (!s_material) return;
    }

    auto* lrp = reinterpret_cast<bedrocktools::sdk::LevelRenderer*>(levelRenderer)->playerRenderer();
    if (!lrp) return;
    const auto camera = lrp->cameraPosition();

    const auto* tessPtr = reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(screenContext) +
        bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessPtr || !*tessPtr) return;
    void* tess = reinterpret_cast<void*>(*tessPtr);

    // Use the same material fallback used by BedrockTools' existing
    // world-overlay renderers. Also set the ScreenContext color holder;
    // without it, the selection material can inherit a zero/old color and
    // make the LOD quads effectively invisible.
    void* material = s_material
        ? reinterpret_cast<void*>(s_material)
        : lrp->selectionOverlayMaterial();
    if (!material) return;

    const auto colorHolderPtr = *reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(screenContext) +
        bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr) return;

    auto* colorHolder = reinterpret_cast<float*>(colorHolderPtr);
    const float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]
    };

    const int radii[3] = {static_cast<int>(m_closeDistance), static_cast<int>(m_farDistance), static_cast<int>(m_fartherDistance)};
    const int resolutions[3] = {std::max(1, m_closeResolution), std::max(1, m_farResolution), std::max(1, m_fartherResolution)};
    const float starts[3] = {0.0f, m_closeDistance, m_farDistance};
    const float ends[3] = {m_closeDistance, m_farDistance, m_fartherDistance};
    const float colors[3][3] = {{0.55f,0.78f,0.98f},{0.47f,0.68f,0.90f},{0.39f,0.57f,0.80f}};

    char pad[0x58]{};
    for (int lod = 0; lod < 3; ++lod) {
        const int resolution = resolutions[lod];
        // BedrockTools' tessellator uses the ScreenContext color holder
        // as an additional modulation path for these immediate meshes.
        colorHolder[0] = 1.0f;
        colorHolder[1] = 1.0f;
        colorHolder[2] = 1.0f;
        colorHolder[3] = 1.0f;

        s_begin(tess, nullptr, 1, static_cast<int>(m_cells.size() * 8), 0);
        for (const auto& [key, cell] : m_cells) {
            if (key.lod != lod || !cell.valid) continue;
            const float x0 = key.x * resolution;
            const float z0 = key.z * resolution;
            const float x1 = x0 + resolution;
            const float z1 = z0 + resolution;
            const float cx = (x0 + x1) * 0.5f - m_playerPosition.x;
            const float cz = (z0 + z1) * 0.5f - m_playerPosition.z;
            const float distance = std::sqrt(cx * cx + cz * cz);
            if (distance < starts[lod] - resolution || distance > ends[lod] + resolution) continue;

            float alpha = m_opacity * (1.0f - fadeFactor(distance, starts[lod], ends[lod], m_transitionWidth));
            if (lod == 0) alpha = std::max(alpha, 0.10f);
            if (!m_smoothLodTransition) alpha = std::clamp(m_opacity, 0.05f, 1.0f);

            float r = colors[lod][0] + cell.slope * 0.10f;
            float g = colors[lod][1] + cell.slope * 0.08f;
            float b = colors[lod][2] + cell.slope * 0.05f;
            s_color(tess, std::clamp(r,0.0f,1.0f), std::clamp(g,0.0f,1.0f), std::clamp(b,0.0f,1.0f), std::clamp(alpha,0.02f,1.0f));
            emitQuad(tess, x0, cell.height + m_surfaceOffset, z0,
                     x1, cell.height + m_surfaceOffset, z0,
                     x1, cell.height + m_surfaceOffset, z1,
                     x0, cell.height + m_surfaceOffset, z1,
                     camera.x, camera.y, camera.z);
        }
        s_renderMesh(screenContext, tess, material, pad);
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];

    (void)radii;
}

void DistantTerrainModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_closeDistance = j.value("m_closeDistance", m_closeDistance);
    m_farDistance = j.value("m_farDistance", m_farDistance);
    m_fartherDistance = j.value("m_fartherDistance", m_fartherDistance);
    m_closeResolution = j.value("m_closeResolution", m_closeResolution);
    m_farResolution = j.value("m_farResolution", m_farResolution);
    m_fartherResolution = j.value("m_fartherResolution", m_fartherResolution);
    m_samplesPerFrame = j.value("m_samplesPerFrame", m_samplesPerFrame);
    m_maxCachedCells = j.value("m_maxCachedCells", m_maxCachedCells);
    m_heightRange = j.value("m_heightRange", m_heightRange);
    m_surfaceOffset = j.value("m_surfaceOffset", m_surfaceOffset);
    m_transitionWidth = j.value("m_transitionWidth", m_transitionWidth);
    m_opacity = j.value("m_opacity", m_opacity);
    m_smoothLodTransition = j.value("m_smoothLodTransition", m_smoothLodTransition);
    m_showWater = j.value("m_showWater", m_showWater);
    m_showVegetationMass = j.value("m_showVegetationMass", m_showVegetationMass);
    m_rebuildButton = j.value("m_rebuildButton", false);

    m_closeDistance = std::clamp(m_closeDistance, 8.0f, 96.0f);
    m_farDistance = std::clamp(m_farDistance, m_closeDistance + 8.0f, 192.0f);
    m_fartherDistance = std::clamp(m_fartherDistance, m_farDistance + 16.0f, 512.0f);
    m_closeResolution = std::clamp(m_closeResolution, 1, 16);
    m_farResolution = std::clamp(m_farResolution, m_closeResolution, 32);
    m_fartherResolution = std::clamp(m_fartherResolution, m_farResolution, 64);
    m_samplesPerFrame = std::clamp(m_samplesPerFrame, 1, 32);
    m_maxCachedCells = std::clamp(m_maxCachedCells, 256, 50000);
    m_heightRange = std::clamp(m_heightRange, 64.0f, 384.0f);
    m_surfaceOffset = std::clamp(m_surfaceOffset, 0.0f, 1.0f);
    m_transitionWidth = std::clamp(m_transitionWidth, 0.0f, 32.0f);
    m_opacity = std::clamp(m_opacity, 0.05f, 1.0f);
}

void DistantTerrainModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_closeDistance"] = m_closeDistance;
    j["m_farDistance"] = m_farDistance;
    j["m_fartherDistance"] = m_fartherDistance;
    j["m_closeResolution"] = m_closeResolution;
    j["m_farResolution"] = m_farResolution;
    j["m_fartherResolution"] = m_fartherResolution;
    j["m_samplesPerFrame"] = m_samplesPerFrame;
    j["m_maxCachedCells"] = m_maxCachedCells;
    j["m_heightRange"] = m_heightRange;
    j["m_surfaceOffset"] = m_surfaceOffset;
    j["m_transitionWidth"] = m_transitionWidth;
    j["m_opacity"] = m_opacity;
    j["m_smoothLodTransition"] = m_smoothLodTransition;
    j["m_showWater"] = m_showWater;
    j["m_showVegetationMass"] = m_showVegetationMass;
    j["m_rebuildButton"] = false;
}
