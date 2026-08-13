#include "paperdoll.hpp"

#include <algorithm>
#include <cstring>
#include <cstdint>

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/memory/Signatures.hpp>

namespace {
PaperDollModule* g_paperDoll = nullptr;

static const void* getSkinImageFromPlayer(void* actor) {
    if (!actor) return nullptr;

    auto skinRef = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(actor) +
        bedrocktools::sdk::offsets::Player::mSkin);
    if (!skinRef) return nullptr;

    auto threadOwner = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(skinRef) +
        bedrocktools::sdk::offsets::SerializedSkinRef::mSkinImpl);
    if (!threadOwner) return nullptr;

    auto skinImpl = reinterpret_cast<uintptr_t>(threadOwner) +
        bedrocktools::sdk::offsets::ThreadOwner::mObject;

    auto image = reinterpret_cast<const void*>(
        skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinImage);

    if (*reinterpret_cast<const bool*>(
            skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mIsPersona)) {
        auto begin = *reinterpret_cast<const uintptr_t*>(
            skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages);
        auto end = *reinterpret_cast<const uintptr_t*>(
            skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages +
            sizeof(uintptr_t));

        if (begin && end >= begin &&
            end - begin <= bedrocktools::sdk::offsets::AnimatedImageData::Size * 64) {
            for (auto entry = begin; entry < end;
                 entry += bedrocktools::sdk::offsets::AnimatedImageData::Size) {
                auto type = *reinterpret_cast<const uint32_t*>(
                    entry + bedrocktools::sdk::offsets::AnimatedImageData::mType);
                if (type == 2 || type == 3) {
                    image = reinterpret_cast<const void*>(
                        entry + bedrocktools::sdk::offsets::AnimatedImageData::mImage);
                    if (type == 3) break;
                }
            }
        }
    }

    return image;
}

static bool getSkinPixels(void* actor, const uint8_t*& pixels,
                          uint32_t& width, uint32_t& height) {
    auto image = getSkinImageFromPlayer(actor);
    if (!image) return false;

    auto addr = reinterpret_cast<uintptr_t>(image);
    width = *reinterpret_cast<const uint32_t*>(
        addr + bedrocktools::sdk::offsets::SkinImage::mWidth);
    height = *reinterpret_cast<const uint32_t*>(
        addr + bedrocktools::sdk::offsets::SkinImage::mHeight);
    pixels = *reinterpret_cast<const uint8_t* const*>(
        addr + bedrocktools::sdk::offsets::Image::mBytesOffset);

    return pixels && width >= 64 && width <= 256 &&
           height >= 32 && height <= 256 &&
           width % 64 == 0 && height % 32 == 0;
}

// Builds a front-facing 2D paper-doll sprite from the loaded Bedrock skin.
// It intentionally uses the existing ModMenu image path rather than a new
// renderer texture API.
static bool buildDoll(void* actor, std::array<uint8_t, 64*96*4>& out) {
    const uint8_t* src = nullptr;
    uint32_t sw = 0, sh = 0;
    if (!getSkinPixels(actor, src, sw, sh)) return false;

    const int scale = static_cast<int>(sw / 64);
    out.fill(0);

    auto copyScaled = [&](int sx, int sy, int w, int h, int dx, int dy) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int px = (sx + x) * scale;
                int py = (sy + y) * scale;
                if (px >= static_cast<int>(sw) || py >= static_cast<int>(sh)) continue;
                const auto* p = src + (static_cast<std::size_t>(py) * sw + px) * 4;

                for (int yy = 0; yy < scale; ++yy) {
                    for (int xx = 0; xx < scale; ++xx) {
                        int ox = dx + x * scale + xx;
                        int oy = dy + y * scale + yy;
                        if (ox < 0 || oy < 0 || ox >= 64 ||
                            oy >= 96) continue;
                        auto* d = out.data() +
                            (static_cast<std::size_t>(oy) * 64 + ox) * 4;
                        std::memcpy(d, p, 4);
                    }
                }
            }
        }
    };

    // Head, body, arms and legs, using the standard 64x64 skin front faces.
    // The resulting sprite is centered and enlarged for a clean HUD doll.
    constexpr int S = 2;
    auto copyPart = [&](int sx, int sy, int w, int h, int dx, int dy) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int px = (sx + x) * scale;
                int py = (sy + y) * scale;
                if (px >= (int)sw || py >= (int)sh) continue;
                const auto* p = src + ((size_t)py * sw + px) * 4;
                for (int yy=0; yy<S; ++yy) for (int xx=0; xx<S; ++xx) {
                    int ox=dx+x*S+xx, oy=dy+y*S+yy;
                    if(ox<0||oy<0||ox>=64||oy>=96) continue;
                    auto* d=out.data()+((size_t)oy*64+ox)*4;
                    std::memcpy(d,p,4);
                }
            }
    };

    // 8x8 head -> 16x16, 8x12 body -> 16x24, 4x12 limbs -> 8x24.
    copyPart(8, 8, 8, 8, 24, 2);
    copyPart(20, 20, 8, 12, 24, 18);
    copyPart(44, 20, 4, 12, 16, 18);
    copyPart(36, 52, 4, 12, 40, 18);
    copyPart(4, 20, 4, 12, 24, 42);
    copyPart(20, 52, 4, 12, 32, 42);

    // Head outer layer: x=40..47, y=8..15.
    copyPart(40, 8, 8, 8, 24, 2);
    return true;
}
}

PaperDollModule::PaperDollModule()
    : Module("Paper Doll", "Displays your player skin as a compact HUD paper doll.") {
    g_paperDoll = this;
    isHudModule = true;
    hudPosX = 40.f;
    hudPosY = 80.f;
}

PaperDollModule::~PaperDollModule() {
    if (g_paperDoll == this) g_paperDoll = nullptr;
}

void PaperDollModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_paperDoll && g_paperDoll->enabled)
                g_paperDoll->onLocalPlayerTick(event.player);
        });
}

void PaperDollModule::onEnable() {
    m_refreshTicks = 0;
    m_hasImage = false;
}

void PaperDollModule::onDisable() {
    m_hasImage = false;
}

void PaperDollModule::onLocalPlayerTick(void* localPlayer) {
    if (!localPlayer) return;
    if (++m_refreshTicks < 10) return;
    m_refreshTicks = 0;
    updateFromPlayer(localPlayer);
}

void PaperDollModule::updateFromPlayer(void* player) {
    Pixels pixels{};
    if (!buildDoll(player, pixels)) return;

    pl::modmenu::registerImage(m_imageId, pixels, TEX_W, TEX_H);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hasImage = true;
}

void PaperDollModule::onFrame() {
    if (!enabled) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasImage) return;

    std::vector<PLModMenu_DrawCommand> cmds;

    const float w = TEX_W * scale;
    const float h = TEX_H * scale;

    if (background) {
        PLModMenu_DrawCommand bg{};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = w;
        bg.h = h;
        bg.color = (static_cast<int>(backgroundOpacity * 255.f) << 24);
        cmds.push_back(bg);
    }

    PLModMenu_DrawCommand image{};
    image.type = PL_DRAW_IMAGE;
    image.x = hudPosX;
    image.y = hudPosY;
    image.w = w;
    image.h = h;
    image.color = 0xFFFFFFFF;
    image.imageId = m_imageId;
    cmds.push_back(image);

    submitDrawCommands(moduleId, cmds);
}

void PaperDollModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    hudPosX = j.value("hudPosX", hudPosX);
    hudPosY = j.value("hudPosY", hudPosY);
    scale = std::clamp(j.value("scale", scale), 0.5f, 4.0f);
    background = j.value("background", background);
    backgroundOpacity = std::clamp(j.value("backgroundOpacity", backgroundOpacity), 0.f, 1.f);
    showHeadLayer = j.value("showHeadLayer", showHeadLayer);
}
void PaperDollModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"]=hudPosX; j["hudPosY"]=hudPosY; j["scale"]=scale;
    j["background"]=background; j["backgroundOpacity"]=backgroundOpacity;
    j["showHeadLayer"]=showHeadLayer;
}
