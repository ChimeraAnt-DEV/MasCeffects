#pragma once

/**
 * @file MasEffectConfig.hpp
 * @brief Typed, persisted configuration for the MasCeffects mod.
 *
 * The struct is the single source of truth shared by the runtime code and the
 * host-side schema generator, exactly as `pl::config::ConfigFile<T>` expects:
 * every field is an aggregate member, so the reflection in `pl/Config.hpp`
 * reads it without hand-written serialization.
 */

#include <optional>
#include <string>
#include <string_view>

#include <pl/Config.hpp>

#include "EffectMath.hpp"

namespace masc {

struct MasEffectConfig {
  /// Schema version managed by the mod; bumped when a field changes meaning.
  int version = 1;

  // ---- Mace smash shockwave ------------------------------------------------
  bool shockwaveEnabled = true;
  int shockwaveSegments = 48;
  double shockwaveRadius = 6.0;
  double shockwaveLifeMs = 700.0;
  double shockwaveThickness = 0.06;
  std::string shockwaveColor = "#7FD4FF";
  /// Spawn a shockwave automatically on a detected hard landing.
  bool shockwaveAutoOnLanding = true;
  /// Downward speed (blocks/second) that counts as a smash landing.
  double shockwaveMinFallSpeed = 14.0;

  // ---- Ender pearl trail ---------------------------------------------------
  bool pearlTrailEnabled = true;
  std::string pearlTrailColor = "#C08CFF";
  double pearlTrailLifeMs = 600.0;
  double pearlTrailWidth = 0.12;
  /// A single-frame horizontal displacement above this (blocks) is a teleport.
  double pearlTrailMinJump = 4.0;

  // ---- Impact burst --------------------------------------------------------
  bool impactBurstEnabled = true;
  std::string impactBurstColor = "#FF4D4D";
  double impactBurstLifeMs = 1200.0;
  int impactBurstParticles = 48;
  double impactBurstRadius = 1.4;

  // ---- Player death effect -------------------------------------------------
  /// Draws a burst and a rising marker when a tracked player dies.
  bool deathEffectEnabled = true;
  std::string deathEffectColor = "#FF4D4D";
  double deathEffectLifeMs = 1500.0;
  int deathEffectParticles = 30;
  double deathEffectRadius = 1.0;
  double deathEffectMarkerSize = 0.35;
  /// Blocks the marker rises over its lifetime.
  double deathEffectRise = 1.2;
  /// A health reading above this is garbage from a wrong offset and is ignored.
  double deathEffectMaxHealth = 20.0;
  /// The most death effects alive at once.
  int deathEffectMaxConcurrent = 8;

  // ---- Hitbox styling ------------------------------------------------------
  /// Draws a box for every entity the game is about to render.
  bool hitboxEnabled = true;
  double hitboxOpacity = 0.8;
  std::string hitboxColor = "#FFFFFF";
  /// Opacity for non-player living entities.
  double hitboxMobOpacity = 0.3;
  /// Opacity of your own box.
  double hitboxSelfOpacity = 0.5;
  bool hitboxShowLookLine = false;
  /// Distance in blocks at which a player/mob box starts to fade out.
  double hitboxFadeDistance = 15.0;
  /// Blocks over which it fades to nothing after that.
  double hitboxFadeRange = 20.0;
  /// The most entity boxes drawn in one pass.
  int hitboxMaxEntities = 128;

  // ---- Status overlay ------------------------------------------------------
  bool showStatusHud = true;
  double statusHudScale = 1.0;
  bool showDiagnostics = false;
};

inline constexpr int kMinSegments = kMinRingSegments;
inline constexpr int kMaxSegments = kMaxRingSegments;
inline constexpr double kMinRadius = 1.0;
inline constexpr double kMaxRadius = 16.0;
inline constexpr double kMinLifeMs = 100.0;
inline constexpr double kMaxLifeMs = 4000.0;
inline constexpr double kMinThickness = 0.01;
inline constexpr double kMaxThickness = 0.5;
inline constexpr double kMinFallSpeed = 1.0;
inline constexpr double kMaxFallSpeed = 80.0;
inline constexpr double kMinJump = 1.0;
inline constexpr double kMaxJump = 64.0;
inline constexpr int kMinParticles = 4;
inline constexpr int kMaxParticles = 200;
inline constexpr double kMinOpacity = 0.05;
inline constexpr double kMaxOpacity = 1.0;
inline constexpr double kMinScale = 0.5;
inline constexpr double kMaxScale = 2.5;
inline constexpr double kMinBox = 0.05;
inline constexpr double kMaxBox = 4.0;
inline constexpr double kMinFadeDistance = 0.0;
inline constexpr double kMaxFadeDistance = 128.0;
inline constexpr double kMinFadeRange = 0.0;
inline constexpr double kMaxFadeRange = 128.0;
inline constexpr double kMinRise = 0.0;
inline constexpr double kMaxRise = 8.0;
inline constexpr int kMinMaxEntities = 1;
inline constexpr int kMaxMaxEntities = 512;
inline constexpr int kMinConcurrent = 1;
inline constexpr int kMaxConcurrent = 32;

/// Colour choices offered by the Mod Menu radio entries, as `#RRGGBB` values.
inline constexpr std::string_view kColorChoices =
    "#7FD4FF,#C08CFF,#FF4D4D,#4AE0A0,#FFC24B,#FFFFFF";
inline constexpr std::string_view kColorChoicesWithNone =
    "Off,#7FD4FF,#C08CFF,#FF4D4D,#4AE0A0,#FFC24B,#FFFFFF";

} // namespace masc

namespace pl::config {

template <> struct Schema<masc::MasEffectConfig> {
  static constexpr std::string_view title = "MasCeffects";
  static constexpr std::string_view description =
      "Visual PvP effects ported from the Mas Effects Java Edition mod: mace "
      "smash shockwave, ender pearl trail, player death effect, and hitbox "
      "styling.";

  static constexpr FieldSchema field(std::string_view name) {
    if (name == "version") {
      return {"Version", "Config schema version managed by the mod.",
              std::nullopt, std::nullopt, true};
    }
    if (name == "shockwaveEnabled") {
      return {"Shockwave", "Draw the mace smash shockwave.", std::nullopt,
              std::nullopt, false};
    }
    if (name == "shockwaveSegments") {
      return {"Shockwave Segments",
              "Vertices around the ring; higher is smoother.", masc::kMinSegments,
              masc::kMaxSegments, false};
    }
    if (name == "shockwaveRadius") {
      return {"Shockwave Radius", "Final ring radius in blocks.",
              masc::kMinRadius, masc::kMaxRadius, false};
    }
    if (name == "shockwaveLifeMs") {
      return {"Shockwave Lifetime", "Ring lifetime in milliseconds.",
              masc::kMinLifeMs, masc::kMaxLifeMs, false};
    }
    if (name == "shockwaveThickness") {
      return {"Shockwave Thickness", "Ring band width in blocks.",
              masc::kMinThickness, masc::kMaxThickness, false};
    }
    if (name == "shockwaveColor") {
      return {"Shockwave Color", "Ring colour, #RRGGBB or #AARRGGBB.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "shockwaveAutoOnLanding") {
      return {"Auto On Landing",
              "Spawn a shockwave when a hard landing is detected.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "shockwaveMinFallSpeed") {
      return {"Min Fall Speed",
              "Downward speed in blocks/second that counts as a smash.",
              masc::kMinFallSpeed, masc::kMaxFallSpeed, false};
    }
    if (name == "pearlTrailEnabled") {
      return {"Pearl Trail", "Draw the ender pearl trail.", std::nullopt,
              std::nullopt, false};
    }
    if (name == "pearlTrailColor") {
      return {"Pearl Trail Color", "Trail colour, #RRGGBB or #AARRGGBB.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "pearlTrailLifeMs") {
      return {"Pearl Trail Lifetime", "Trail lifetime in milliseconds.",
              masc::kMinLifeMs, masc::kMaxLifeMs, false};
    }
    if (name == "pearlTrailWidth") {
      return {"Pearl Trail Width", "Ribbon width in blocks.",
              masc::kMinThickness, masc::kMaxThickness, false};
    }
    if (name == "pearlTrailMinJump") {
      return {"Teleport Threshold",
              "Single-frame displacement in blocks treated as a teleport.",
              masc::kMinJump, masc::kMaxJump, false};
    }
    if (name == "impactBurstEnabled") {
      return {"Impact Burst", "Draw the impact burst.", std::nullopt,
              std::nullopt, false};
    }
    if (name == "impactBurstColor") {
      return {"Impact Burst Color", "Burst colour, #RRGGBB or #AARRGGBB.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "impactBurstLifeMs") {
      return {"Impact Burst Lifetime", "Burst lifetime in milliseconds.",
              masc::kMinLifeMs, masc::kMaxLifeMs, false};
    }
    if (name == "impactBurstParticles") {
      return {"Impact Burst Particles", "Particles per burst.",
              masc::kMinParticles, masc::kMaxParticles, false};
    }
    if (name == "impactBurstRadius") {
      return {"Impact Burst Radius", "Burst radius in blocks.",
              masc::kMinRadius, masc::kMaxRadius, false};
    }
    if (name == "deathEffectEnabled") {
      return {"Player Death Effect",
              "Draw a burst and a rising marker when a player dies.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "deathEffectColor") {
      return {"Death Effect Color", "Burst colour, #RRGGBB or #AARRGGBB.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "deathEffectLifeMs") {
      return {"Death Effect Lifetime", "Effect lifetime in milliseconds.",
              masc::kMinLifeMs, masc::kMaxLifeMs, false};
    }
    if (name == "deathEffectParticles") {
      return {"Death Effect Particles", "Particles per death burst.",
              masc::kMinParticles, masc::kMaxParticles, false};
    }
    if (name == "deathEffectRadius") {
      return {"Death Effect Radius", "Burst radius in blocks.",
              masc::kMinRadius, masc::kMaxRadius, false};
    }
    if (name == "deathEffectMarkerSize") {
      return {"Death Marker Size", "Size of the rising marker in blocks.",
              masc::kMinBox, masc::kMaxBox, false};
    }
    if (name == "deathEffectRise") {
      return {"Death Marker Rise",
              "Blocks the marker rises over its lifetime.", masc::kMinRise,
              masc::kMaxRise, false};
    }
    if (name == "deathEffectMaxHealth") {
      return {"Max Plausible Health",
              "A health reading above this is treated as garbage and ignored.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "deathEffectMaxConcurrent") {
      return {"Max Death Effects", "The most death effects alive at once.",
              masc::kMinConcurrent, masc::kMaxConcurrent, false};
    }
    if (name == "hitboxEnabled") {
      return {"Custom Hitboxes",
              "Draw a box for every entity the game renders.", std::nullopt,
              std::nullopt, false};
    }
    if (name == "hitboxOpacity") {
      return {"Player Hitbox Opacity", "Opacity of player boxes.",
              masc::kMinOpacity, masc::kMaxOpacity, false};
    }
    if (name == "hitboxColor") {
      return {"Player Hitbox Color", "Player box colour, #RRGGBB or #AARRGGBB.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "hitboxMobOpacity") {
      return {"Mob Hitbox Opacity",
              "Opacity of non-player living entity boxes.", masc::kMinOpacity,
              masc::kMaxOpacity, false};
    }
    if (name == "hitboxSelfOpacity") {
      return {"Self Hitbox Opacity", "Opacity of your own box.",
              masc::kMinOpacity, masc::kMaxOpacity, false};
    }
    if (name == "hitboxShowLookLine") {
      return {"Hitbox Look Line", "Draw a view direction line from each box.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "hitboxFadeDistance") {
      return {"Hitbox Fade Distance",
              "Distance in blocks at which a box starts to fade out.",
              masc::kMinFadeDistance, masc::kMaxFadeDistance, false};
    }
    if (name == "hitboxFadeRange") {
      return {"Hitbox Fade Range",
              "Blocks over which a box fades to nothing after that.",
              masc::kMinFadeRange, masc::kMaxFadeRange, false};
    }
    if (name == "hitboxMaxEntities") {
      return {"Max Hitboxes",
              "The most entity boxes drawn in one pass.", masc::kMinMaxEntities,
              masc::kMaxMaxEntities, false};
    }
    if (name == "showStatusHud") {
      return {"Status HUD",
              "Show the small effect/diagnostic readout in the corner.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "statusHudScale") {
      return {"Status HUD Scale", "Readout size multiplier.", masc::kMinScale,
              masc::kMaxScale, false};
    }
    if (name == "showDiagnostics") {
      return {"Diagnostics",
              "Add render-seam and GL diagnostics to the status HUD.",
              std::nullopt, std::nullopt, false};
    }
    return {};
  }
};

} // namespace pl::config
