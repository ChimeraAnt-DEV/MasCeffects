#pragma once

/**
 * @file EffectEngine.hpp
 * @brief Owns the live effect list and turns motion events into geometry.
 *
 * The render callback runs once per rendered player model, so it is not a frame
 * boundary. This class therefore keys on the render tick: it advances the
 * simulation only when the tick changes, which keeps every effect advancing at
 * real time even though the callback may run several times in one frame.
 *
 * <p>All state is written and read on the render thread, so there is no lock
 * here. The config snapshot is taken under the owner's mutex before each pass.
 */

#include <cstdint>
#include <vector>

#include "EffectMath.hpp"
#include "GameFeed.hpp"
#include "MasEffectConfig.hpp"

namespace masc {

/// A fully resolved config, copied under lock so the render thread never races
/// with a Mod Menu edit.
struct ConfigSnapshot {
  bool shockwaveEnabled = true;
  int shockwaveSegments = 48;
  double shockwaveRadius = 6.0;
  double shockwaveLifeMs = 700.0;
  double shockwaveThickness = 0.06;
  Rgba shockwaveColor = 0xFF7FD4FFu;
  bool shockwaveAutoOnLanding = true;
  double shockwaveMinFallSpeed = 14.0;

  bool pearlTrailEnabled = true;
  Rgba pearlTrailColor = 0xFFC08CFFu;
  double pearlTrailLifeMs = 600.0;
  double pearlTrailWidth = 0.12;
  double pearlTrailMinJump = 4.0;

  bool impactBurstEnabled = true;
  Rgba impactBurstColor = 0xFFFF4D4Du;
  double impactBurstLifeMs = 1200.0;
  int impactBurstParticles = 48;
  double impactBurstRadius = 1.4;

  bool deathEffectEnabled = true;
  Rgba deathEffectColor = 0xFFFF4D4Du;
  double deathEffectLifeMs = 1500.0;
  int deathEffectParticles = 30;
  double deathEffectRadius = 1.0;
  double deathEffectMarkerSize = 0.35;
  double deathEffectRise = 1.2;
  double deathEffectMaxHealth = 20.0;
  int deathEffectMaxConcurrent = 8;

  bool hitboxEnabled = true;
  double hitboxOpacity = 0.8;
  Rgba hitboxColor = 0xFFFFFFFFu;
  double hitboxMobOpacity = 0.3;
  double hitboxSelfOpacity = 0.5;
  bool hitboxShowLookLine = false;
  double hitboxFadeDistance = 15.0;
  double hitboxFadeRange = 20.0;
  int hitboxMaxEntities = 128;

  bool showStatusHud = true;
  double statusHudScale = 1.0;
  bool showDiagnostics = false;

  /// Builds a snapshot from the persisted config, parsing colours once.
  static ConfigSnapshot From(const MasEffectConfig &config);
};

/** @brief One thing to draw this pass: geometry, colour and world-space state. */
struct DrawBatch {
  std::vector<Segment> segments;
  Rgba color = 0xFFFFFFFFu;
};

/**
 * @brief The effect simulation.
 *
 * Effects are held in one flat list; a pass produces one batch per live effect.
 * The list is bounded, so a long fight cannot grow it without limit.
 */
class EffectEngine {
public:
  /// The most effects alive at once; the oldest is dropped past this.
  static constexpr std::size_t kMaxEffects = 32;
  /// The most trail points retained; the oldest is dropped past this.
  static constexpr std::size_t kMaxTrailPoints = 64;
  /// The most entities whose health is tracked; the oldest is dropped past this.
  static constexpr std::size_t kMaxTrackedEntities = 256;
  /** @brief Drops every effect and the motion baseline. */
  void reset();

  /**
   * @brief Advances the simulation to @p nowMs and records new events.
   *
   * @param tick       the render tick; a repeated tick is ignored so the
   *                   simulation advances once per frame, not once per call
   * @param nowMs      monotonic milliseconds
   * @param playerX/Y/Z the local player's current position
   * @param config     the resolved configuration
   */
  void update(std::uint64_t tick, std::int64_t nowMs, float playerX,
              float playerY, float playerZ, const ConfigSnapshot &config);

  /**
   * @brief Produces the geometry to draw at @p nowMs.
   *
   * Batches are rebuilt from scratch each pass; an effect whose lifetime has
   * expired contributes nothing and is dropped by the next {@link update}.
   */
  std::vector<DrawBatch> collect(std::int64_t nowMs,
                                 const ConfigSnapshot &config);

  /** @brief The number of live effects, for the status HUD. */
  [[nodiscard]] std::size_t liveCount() const { return effects_.size(); }

  /** @brief The number of smash events seen this session. */
  [[nodiscard]] std::uint64_t smashCount() const { return smashCount_; }

  /** @brief The number of teleport events seen this session. */
  [[nodiscard]] std::uint64_t teleportCount() const { return teleportCount_; }

  /** @brief The number of deaths detected this session. */
  [[nodiscard]] std::uint64_t deathCount() const { return deathCount_; }

  /** @brief Forgets every tracked entity health history. */
  void resetHealthTracking() { health_.clear(); }

  /**
   * @brief Feeds one frame of tracked entities and spawns death effects.
   *
   * A death is a tracked entity whose health was a plausible positive value and
   * has now reached zero. An entity whose health was never readable never fires,
   * so a wrong health offset produces no effect rather than a false one. The
   * debounce collapses the several frames the death screen reports zero into one
   * trigger.
   *
   * @param entities  the entities captured this frame
   * @param config    the resolved configuration
   * @param nowMs     monotonic milliseconds
   */
  void observeEntities(const std::vector<GameEntity> &entities,
                       const ConfigSnapshot &config, std::int64_t nowMs);

  /**
   * @brief Spawns a shockwave immediately, for the Mod Menu test button.
   *
   * Runs on the render thread, so it takes the same path a detected landing
   * does and is bounded by the same effect limit.
   */
  void requestShockwave(float x, float y, float z, const ConfigSnapshot &config,
                        std::int64_t nowMs);

private:
  enum class EffectKind { Shockwave, ImpactBurst, Death };

  /// Per-entity health history, so a drop to zero can be told from a source that
  /// only ever reported zero.
  struct HealthTrack {
    std::uintptr_t entity = 0;
    bool sawLiveHealth = false;
    std::int64_t lastDeathMs = 0;
    bool firedDeath = false;
  };

  struct Effect {
    EffectKind kind = EffectKind::Shockwave;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    std::int64_t bornMs = 0;
    double lifeMs = 1.0;
    Rgba color = 0xFFFFFFFFu;
    double radius = 1.0;
    int segments = 48;
    double thickness = 0.06;
    int particles = 48;
    std::uint32_t seed = 1;
    /// Death effects only: the marker's size and how far it rises.
    double markerSize = 0.35;
    double rise = 1.2;
  };

  struct TrailPoint {
    Point point{};
    std::int64_t stampMs = 0;
  };

  void spawnSmash(float x, float y, float z, const ConfigSnapshot &config,
                  std::int64_t nowMs);
  void spawnTeleport(float fromX, float fromY, float fromZ, float toX,
                     float toY, float toZ, const ConfigSnapshot &config,
                     std::int64_t nowMs);
  void spawnDeath(float x, float y, float z, const ConfigSnapshot &config,
                  std::int64_t nowMs);
  void dropExpired(std::int64_t nowMs);

  std::vector<Effect> effects_;
  std::vector<TrailPoint> trail_;
  std::vector<HealthTrack> health_;
  MotionTracker tracker_;
  std::uint64_t lastTick_ = 0;
  bool hasTick_ = false;
  std::int64_t lastUpdateMs_ = 0;
  std::uint32_t seed_ = 0x9E3779B9u;
  std::uint64_t smashCount_ = 0;
  std::uint64_t teleportCount_ = 0;
  std::uint64_t deathCount_ = 0;
};

} // namespace masc
