#include "locatorheads.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
constexpr int HEAD_TEX_SIZE = 64;
using HeadPixels = std::array<std::uint8_t, HEAD_TEX_SIZE * HEAD_TEX_SIZE * 4>;

using GetRuntimeActorList_t = std::vector<void*>(*)(void*);
using ActorIsPlayer_t = bool(*)(void*);
using ActorGetNameTag_t = std::string(*)(void*);

GetRuntimeActorList_t s_getRuntimeActorList = nullptr;
ActorIsPlayer_t s_actorIsPlayer = nullptr;
ActorGetNameTag_t s_actorGetNameTag = nullptr;

static std::string cleanPlayerName(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xA7) {
            i += 2;
            if (i < input.size()) ++i;
            continue;
        }
        if (c == 0xA7) {
            i += std::min<std::size_t>(2, input.size() - i);
            continue;
        }
        if (c >= 0x20 && c != 0x7F) output.push_back(input[i]);
        ++i;
    }

    const auto first = output.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = output.find_last_not_of(' ');
    output = output.substr(first, last - first + 1);
    if (output.size() > 128) output.resize(128);
    return output;
}

static const void* getSkinImageFromActor(void* actor) {
    if (!actor) return nullptr;

    auto skinRef = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Player::mSkin);
    if (!skinRef) return nullptr;

    auto threadOwner = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(skinRef) + bedrocktools::sdk::offsets::SerializedSkinRef::mSkinImpl);
    if (!threadOwner) return nullptr;

    auto skinImpl = reinterpret_cast<std::uintptr_t>(threadOwner) + bedrocktools::sdk::offsets::ThreadOwner::mObject;
    auto image = reinterpret_cast<const void*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinImage);

    if (*reinterpret_cast<const bool*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mIsPersona)) {
        auto begin = *reinterpret_cast<const std::uintptr_t*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages);
        auto end = *reinterpret_cast<const std::uintptr_t*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages + sizeof(std::uintptr_t));
        if (begin && end >= begin && end - begin <= bedrocktools::sdk::offsets::AnimatedImageData::Size * 64) {
            for (auto entry = begin; entry < end; entry += bedrocktools::sdk::offsets::AnimatedImageData::Size) {
                auto type = *reinterpret_cast<const std::uint32_t*>(entry + bedrocktools::sdk::offsets::AnimatedImageData::mType);
                if (type == 2 || type == 3) {
                    image = reinterpret_cast<const void*>(entry + bedrocktools::sdk::offsets::AnimatedImageData::mImage);
                    if (type == 3) break;
                }
            }
        }
    }

    return image;
}

static bool extractHeadFromActor(void* actor, HeadPixels& out) {
    auto image = getSkinImageFromActor(actor);
    if (!image) return false;

    const auto imageAddr = reinterpret_cast<std::uintptr_t>(image);
    const auto width = *reinterpret_cast<const std::uint32_t*>(imageAddr + bedrocktools::sdk::offsets::SkinImage::mWidth);
    const auto height = *reinterpret_cast<const std::uint32_t*>(imageAddr + bedrocktools::sdk::offsets::SkinImage::mHeight);
    const auto pixels = *reinterpret_cast<const std::uint8_t* const*>(imageAddr + bedrocktools::sdk::offsets::Image::mBytesOffset);

    if (!pixels || width < 64 || width > 256 || height < 32 || height > 256 || width % 64 != 0 || height % 32 != 0)
        return false;

    const auto scale = width / 64;
    const int upScale = HEAD_TEX_SIZE / 8;
    out.fill(0);

    auto copyLayer = [&](int skinX, bool overlay) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const int srcX = skinX + x * static_cast<int>(scale) + static_cast<int>(scale / 2);
                const int srcY = 8 * static_cast<int>(scale) + y * static_cast<int>(scale) + static_cast<int>(scale / 2);
                const auto* src = pixels + (static_cast<std::size_t>(srcY) * width + srcX) * 4;

                for (int sy = 0; sy < upScale; ++sy) {
                    for (int sx = 0; sx < upScale; ++sx) {
                        auto* dst = out.data() + ((y * upScale + sy) * HEAD_TEX_SIZE + x * upScale + sx) * 4;
                        if (!overlay) {
                            std::memcpy(dst, src, 4);
                            continue;
                        }

                        const auto alpha = src[3];
                        if (alpha == 0) continue;
                        if (alpha == 255) {
                            std::memcpy(dst, src, 4);
                            continue;
                        }

                        const float a = alpha / 255.0f;
                        const float inverse = 1.0f - a;
                        dst[0] = static_cast<std::uint8_t>(src[0] * a + dst[0] * inverse + 0.5f);
                        dst[1] = static_cast<std::uint8_t>(src[1] * a + dst[1] * inverse + 0.5f);
                        dst[2] = static_cast<std::uint8_t>(src[2] * a + dst[2] * inverse + 0.5f);
                        dst[3] = 255;
                    }
                }
            }
        }
    };

    // Standard Bedrock skin layout: base head at (8, 8), overlay at (40, 8).
    copyLayer(8 * static_cast<int>(scale), false);
    copyLayer(40 * static_cast<int>(scale), true);
    return true;
}

static std::string imageKeyForActor(void* actor) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "locator_head_%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(actor)));
    return buffer;
}

static float wrapDegrees(float value) {
    while (value > 180.f) value -= 360.f;
    while (value < -180.f) value += 360.f;
    return value;
}

static float distance3(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}

static float bearingFromNorth(const bedrocktools::sdk::Vec3& from, const bedrocktools::sdk::Vec3& to) {
    constexpr float radToDeg = 57.29577951308232f;
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    float bearing = std::atan2(dx, dz) * radToDeg;
    if (bearing < 0.f) bearing += 360.f;
    return bearing;
}

static uint32_t withAlpha(uint32_t rgb, float alpha) {
    alpha = std::clamp(alpha, 0.f, 1.f);
    return (rgb & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha * 255.f) << 24);
}

static const char* cardinalFor(int degrees) {
    degrees %= 360;
    if (degrees < 0) degrees += 360;
    switch (degrees) {
        case 0: return "N";
        case 90: return "E";
        case 180: return "S";
        case 270: return "W";
        default: return nullptr;
    }
}
}

LocatorHeadsModule::LocatorHeadsModule()
    : Module("Locator Heads", "Shows nearby players on a locator bar using their actual skin heads.") {}

LocatorHeadsModule::~LocatorHeadsModule() = default;

void LocatorHeadsModule::onInit() {
    s_getRuntimeActorList = reinterpret_cast<GetRuntimeActorList_t>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    s_actorIsPlayer = reinterpret_cast<ActorIsPlayer_t>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    s_actorGetNameTag = reinterpret_cast<ActorGetNameTag_t>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (auto* module = ModuleRegistry::get().find("bedrocktools.Locator Heads")) {
            static_cast<LocatorHeadsModule*>(module)->onLocalPlayerTick(event.player);
        }
    });
}

void LocatorHeadsModule::onEnable() {
    m_refreshTicks = 20;
}

void LocatorHeadsModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_players.clear();
    m_localPlayer = nullptr;
}

void LocatorHeadsModule::refreshPlayers(void* localPlayer) {
    if (!localPlayer || !s_getRuntimeActorList || !s_actorIsPlayer) return;

    auto* local = reinterpret_cast<bedrocktools::sdk::Player*>(localPlayer);
    const auto localPos = local->position();

    auto level = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(localPlayer) + bedrocktools::sdk::offsets::Actor::mLevel);
    if (!level) return;
    auto actorManager = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(level) + bedrocktools::sdk::offsets::Level::mActorManager);
    if (!actorManager) return;

    const auto actors = s_getRuntimeActorList(actorManager);
    std::vector<PlayerMarker> next;
    next.reserve(actors.size());
    std::unordered_set<void*> seen;

    for (auto* actor : actors) {
        if (!actor || actor == localPlayer || !seen.emplace(actor).second) continue;
        if (!s_actorIsPlayer(actor)) continue;

        const auto* player = reinterpret_cast<const bedrocktools::sdk::Player*>(actor);
        const auto pos = player->position();
        if (distance3(localPos, pos) > m_maxDistance) continue;

        std::string name;
        if (s_actorGetNameTag) name = cleanPlayerName(s_actorGetNameTag(actor));
        if (name.empty()) name = cleanPlayerName(player->name());
        if (name.empty()) continue;

        auto imageKey = imageKeyForActor(actor);
        HeadPixels head{};
        if (extractHeadFromActor(actor, head)) {
            pl::modmenu::registerImage(imageKey, head, HEAD_TEX_SIZE, HEAD_TEX_SIZE);
        } else {
            imageKey.clear();
        }

        next.push_back({actor, std::move(name), std::move(imageKey)});
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_players.swap(next);
    m_localPlayer = localPlayer;
}

void LocatorHeadsModule::onLocalPlayerTick(void* localPlayer) {
    if (!enabled || !localPlayer) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_localPlayer = localPlayer;
        m_localYaw = reinterpret_cast<bedrocktools::sdk::Player*>(localPlayer)->rotation().y;
    }

    if (++m_refreshTicks < 5) return;
    m_refreshTicks = 0;
    refreshPlayers(localPlayer);
}

void LocatorHeadsModule::onFrame() {
    if (!enabled) return;

    void* localPtr = nullptr;
    float localYaw = 0.f;
    std::vector<PlayerMarker> players;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        localPtr = m_localPlayer;
        localYaw = m_localYaw;
        players = m_players;
    }
    if (!localPtr) return;

    auto* local = reinterpret_cast<bedrocktools::sdk::Player*>(localPtr);
    const auto localPos = local->position();

    const float barW = std::max(100.f, m_barWidth);
    const float barH = std::max(12.f, m_barHeight);
    const float centerX = hudPosX + barW * 0.5f;
    const float centerY = hudPosY + barH * 0.5f;
    const float halfW = barW * 0.5f;

    std::vector<PLModMenu_DrawCommand> cmds;
    cmds.reserve(players.size() * 4 + 8);
    std::vector<std::string> transientLabels;
    transientLabels.reserve(players.size());

    PLModMenu_DrawCommand bg{};
    bg.type = PL_DRAW_RECT_FILLED;
    bg.x = hudPosX;
    bg.y = hudPosY;
    bg.w = barW;
    bg.h = barH;
    bg.color = withAlpha(0x101318u, m_backgroundOpacity);
    cmds.push_back(bg);

    // Center marker and cardinal directions make the bar readable even when no player is nearby.
    PLModMenu_DrawCommand center{};
    center.type = PL_DRAW_LINE;
    center.x = centerX;
    center.y = hudPosY + 2.f;
    center.w = 0.f;
    center.h = barH - 4.f;
    center.size = 2.f;
    center.color = withAlpha(0xFFFFFFFFu, m_opacity * 0.85f);
    cmds.push_back(center);

    if (m_showCardinals) {
        struct Cardinal { const char* text; float bearing; } cardinals[] = {
            {"N", 0.f}, {"E", 90.f}, {"S", 180.f}, {"W", 270.f}
        };
        float cameraBearing = std::fmod(localYaw + 180.f, 360.f);
        if (cameraBearing < 0.f) cameraBearing += 360.f;

        for (const auto& cardinal : cardinals) {
            const float rel = wrapDegrees(cardinal.bearing - cameraBearing);
            if (std::fabs(rel) > m_range) continue;
            const float x = centerX + (rel / m_range) * halfW;

            PLModMenu_DrawCommand text{};
            text.type = PL_DRAW_TEXT;
            text.x = x;
            text.y = hudPosY - 18.f;
            text.w = 0.f;
            text.h = 1.f;
            text.size = 12.f;
            text.color = withAlpha(0xFFFFFFFFu, m_opacity * 0.75f);
            text.text = cardinal.text;
            cmds.push_back(text);
        }
    }

    for (const auto& marker : players) {
        if (!marker.actor || marker.imageId.empty()) continue;

        const auto* player = reinterpret_cast<const bedrocktools::sdk::Player*>(marker.actor);
        const auto pos = player->position();
        const float distance = distance3(localPos, pos);
        if (distance > m_maxDistance) continue;

        const float targetBearing = bearingFromNorth(localPos, pos);
        const float cameraBearing = std::fmod(localYaw + 180.f, 360.f);
        const float relative = wrapDegrees(targetBearing - cameraBearing);
        if (std::fabs(relative) > m_range) continue;

        const float x = centerX + (relative / m_range) * halfW;
        const float distanceAlpha = m_fadeWithDistance ? std::clamp(1.f - distance / std::max(1.f, m_maxDistance), 0.25f, 1.f) : 1.f;
        const float alpha = m_opacity * distanceAlpha;
        const float size = std::max(8.f, m_markerSize);

        PLModMenu_DrawCommand head{};
        head.type = PL_DRAW_IMAGE;
        head.x = x - size * 0.5f;
        head.y = centerY - size * 0.5f;
        head.w = size;
        head.h = size;
        head.color = withAlpha(0xFFFFFFFFu, alpha);
        head.imageId = marker.imageId;
        cmds.push_back(head);

        if (m_showVerticalArrows) {
            const float dy = pos.y - localPos.y;
            if (std::fabs(dy) > 4.f) {
                PLModMenu_DrawCommand arrow{};
                arrow.type = PL_DRAW_TEXT;
                arrow.x = x;
                arrow.y = dy > 0.f ? hudPosY - 32.f : hudPosY + barH + 2.f;
                arrow.w = 0.f;
                arrow.h = 1.f;
                arrow.size = 11.f;
                arrow.color = withAlpha(0xFFFFFFFFu, alpha);
                arrow.text = dy > 0.f ? "↑" : "↓";
                cmds.push_back(arrow);
            }
        }

        if (m_showNames || m_showDistance) {
            std::string label;
            if (m_showNames) label = marker.name;
            if (m_showDistance) {
                char distanceText[32];
                std::snprintf(distanceText, sizeof(distanceText), "%.0fm", distance);
                if (!label.empty()) label += " ";
                label += distanceText;
            }

            transientLabels.push_back(std::move(label));

            PLModMenu_DrawCommand text{};
            text.type = PL_DRAW_TEXT;
            text.x = x;
            text.y = hudPosY + barH + 4.f;
            text.w = 0.f;
            text.h = 1.f;
            text.size = 10.f;
            text.color = withAlpha(0xFFFFFFFFu, alpha);
            text.text = transientLabels.back().c_str();
            cmds.push_back(text);
        }
    }

    submitDrawCommands(moduleId, cmds);
}

void LocatorHeadsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_barWidth")) m_barWidth = j["m_barWidth"].get<float>();
    if (j.contains("m_barHeight")) m_barHeight = j["m_barHeight"].get<float>();
    if (j.contains("m_markerSize")) m_markerSize = j["m_markerSize"].get<float>();
    if (j.contains("m_range")) m_range = j["m_range"].get<float>();
    if (j.contains("m_maxDistance")) m_maxDistance = j["m_maxDistance"].get<float>();
    if (j.contains("m_opacity")) m_opacity = j["m_opacity"].get<float>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_showNames")) m_showNames = j["m_showNames"].get<bool>();
    if (j.contains("m_showDistance")) m_showDistance = j["m_showDistance"].get<bool>();
    if (j.contains("m_showVerticalArrows")) m_showVerticalArrows = j["m_showVerticalArrows"].get<bool>();
    if (j.contains("m_fadeWithDistance")) m_fadeWithDistance = j["m_fadeWithDistance"].get<bool>();
    if (j.contains("m_showCardinals")) m_showCardinals = j["m_showCardinals"].get<bool>();
}

void LocatorHeadsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_barWidth"] = m_barWidth;
    j["m_barHeight"] = m_barHeight;
    j["m_markerSize"] = m_markerSize;
    j["m_range"] = m_range;
    j["m_maxDistance"] = m_maxDistance;
    j["m_opacity"] = m_opacity;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_showNames"] = m_showNames;
    j["m_showDistance"] = m_showDistance;
    j["m_showVerticalArrows"] = m_showVerticalArrows;
    j["m_fadeWithDistance"] = m_fadeWithDistance;
    j["m_showCardinals"] = m_showCardinals;
}
