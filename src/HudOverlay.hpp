#pragma once

/**
 * @file HudOverlay.hpp
 * @brief Builds the 2D status readout submitted as Mod Menu draw commands.
 *
 * The Mod Menu's draw-command list is rendered by the launcher's own Android
 * view on top of the game, so it is the right seam for text and flat panels and
 * the wrong one for world-space geometry. Everything here is therefore 2D
 * screen-space; the effects themselves are drawn by {@link WorldRenderer}.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <pl/ModMenu.hpp>

namespace masc {

/** @brief The numbers the status readout reports. */
struct HudStats {
  bool renderLive = false;
  bool pipelineReady = false;
  std::uint64_t frameTick = 0;
  std::uint64_t smashCount = 0;
  std::uint64_t teleportCount = 0;
  std::uint64_t deathCount = 0;
  std::size_t liveEffects = 0;
  /// True once the entity feed has captured something this session.
  bool entityFeedLive = false;
  /// Why the entity feed is unavailable, when it is.
  std::string entityFeedStatus;
  std::uint64_t capturedEntities = 0;
  /// The GL backend name, or "unknown".
  std::string backend;
  int lastGlError = 0;
  bool showDiagnostics = false;
};

/** @brief A registered TrueType font, or none when registration failed. */
class HudOverlay {
public:
  /**
   * @brief Registers the overlay font, once.
   *
   * Text draw commands need a font; without one the readout falls back to a
   * bar. The first call does the file work; later calls are a cheap no-op, so
   * this is safe to call on every HUD tick.
   */
  bool EnsureFont();

  /** @brief True once a font is registered. */
  [[nodiscard]] bool ready() const { return ready_; }

  /**
   * @brief Builds the readout commands for @p stats.
   *
   * @param scale size multiplier applied to the whole readout
   * @return the commands, empty when the font is unavailable
   */
  std::vector<pl::modmenu::DrawCommand> build(const HudStats &stats,
                                              double scale) const;

private:
  bool ready_ = false;
  /// Set once a load has been attempted, so a machine with no readable system
  /// font does not retry the file work on every HUD tick.
  bool fontAttempted_ = false;
};

} // namespace masc
