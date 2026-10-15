#include "HudOverlay.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

#include <pl/Logger.hpp>

#include "EffectMath.hpp"

namespace masc {
namespace {

constexpr const char *kFontId = "masc.status.font";
constexpr float kOriginX = 12.0F;
constexpr float kOriginY = 48.0F;
constexpr float kLineHeight = 14.0F;
constexpr float kTextSize = 11.0F;
constexpr Rgba kTextColor = 0xFFE8ECF2u;
constexpr Rgba kDimColor = 0xFF9AA0A6u;

/// TrueType fonts the platform ships. Reading one avoids bundling a font asset
/// (and its licence) in the mod package.
constexpr const char *kFontCandidates[] = {
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/NotoSans-Regular.ttf",
    "/system/fonts/DroidSans.ttf",
};

/// Caps the read so a pathological file cannot exhaust memory.
constexpr std::size_t kMaxFontBytes = 4u * 1024u * 1024u;

std::vector<unsigned char> ReadFontFile(const char *path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  stream.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(stream.tellg());
  if (size == 0 || size > kMaxFontBytes) return {};
  stream.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(size);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(size));
  if (!stream) return {};
  return bytes;
}

pl::modmenu::DrawCommand Text(std::string text, float x, float y, float size,
                             Rgba color) {
  pl::modmenu::DrawCommand command;
  command.type = pl::modmenu::DrawCommandType::Text;
  command.x = x;
  command.y = y;
  command.color = color;
  command.size = size;
  command.text = std::move(text);
  command.fontId = kFontId;
  return command;
}

/// The no-font fallback: a small bar whose fill tracks the live effect count, so
/// the user still gets feedback when no system font was readable.
std::vector<pl::modmenu::DrawCommand> FallbackIndicator(const HudStats &stats) {
  constexpr float kBarWidth = 4.0F;
  constexpr float kBarHeight = 40.0F;
  const float filled = static_cast<float>(
                           std::min<std::size_t>(stats.liveEffects, 10)) /
                       10.0F;

  pl::modmenu::DrawCommand track;
  track.type = pl::modmenu::DrawCommandType::RectFilled;
  track.x = kOriginX;
  track.y = kOriginY;
  track.w = kBarWidth;
  track.h = kBarHeight;
  track.color = 0x40000000u;

  pl::modmenu::DrawCommand fill;
  fill.type = pl::modmenu::DrawCommandType::RectFilled;
  fill.x = kOriginX;
  fill.y = kOriginY + kBarHeight * (1.0F - filled);
  fill.w = kBarWidth;
  fill.h = kBarHeight * filled;
  fill.color = stats.renderLive ? 0xFF4AE0A0u : 0xFF9AA0A6u;

  return {track, fill};
}

} // namespace

bool HudOverlay::EnsureFont() {
  if (ready_) return true;
  if (fontAttempted_) return false;
  fontAttempted_ = true;

  for (const char *path : kFontCandidates) {
    const std::vector<unsigned char> bytes = ReadFontFile(path);
    if (bytes.empty()) continue;
    if (pl::modmenu::registerFont(kFontId, bytes)) {
      ready_ = true;
      preloaderLogger.info("MasCeffects: status HUD font loaded from {}",
                               path);
      return true;
    }
  }

  preloaderLogger.warn(
      "MasCeffects: no readable system font; status HUD falls back to a bar");
  return false;
}

std::vector<pl::modmenu::DrawCommand> HudOverlay::build(
    const HudStats &stats, double scale) const {
  if (!ready_) return FallbackIndicator(stats);

  const float clamped = static_cast<float>(std::clamp(scale, 0.5, 2.5));
  const float size = kTextSize * clamped;
  const float step = kLineHeight * clamped;
  float y = kOriginY;

  std::vector<pl::modmenu::DrawCommand> commands;
  commands.reserve(10);
  commands.push_back(Text("MasCeffects", kOriginX, y, size * 1.1F, kTextColor));
  y += step;

  commands.push_back(
      Text(stats.renderLive ? "render seam: live" : "render seam: inactive",
           kOriginX, y, size, stats.renderLive ? kTextColor : kDimColor));
  y += step;

  commands.push_back(
      Text("effects: " + std::to_string(stats.liveEffects), kOriginX, y, size,
           kTextColor));
  y += step;

  commands.push_back(Text("smash: " + std::to_string(stats.smashCount) +
                              "   jumps: " +
                              std::to_string(stats.teleportCount),
                          kOriginX, y, size, kTextColor));
  y += step;

  commands.push_back(Text("deaths: " + std::to_string(stats.deathCount) +
                              "   entities: " +
                              std::to_string(stats.capturedEntities),
                          kOriginX, y, size, kTextColor));
  y += step;

  if (stats.showDiagnostics) {
    commands.push_back(
        Text("feed: " + std::string(stats.entityFeedLive ? "live" : "inactive") +
                 " (" + stats.entityFeedStatus + ")",
             kOriginX, y, size, kDimColor));
    y += step;
    commands.push_back(
        Text("gl: " + stats.backend, kOriginX, y, size, kDimColor));
    y += step;
    commands.push_back(
        Text("pipeline: " +
                 std::string(stats.pipelineReady ? "ready" : "unavailable"),
             kOriginX, y, size, kDimColor));
    y += step;
    commands.push_back(
        Text("tick: " + std::to_string(stats.frameTick) +
                 "   glerr: " + std::to_string(stats.lastGlError),
             kOriginX, y, size, kDimColor));
  }

  return commands;
}

} // namespace masc
