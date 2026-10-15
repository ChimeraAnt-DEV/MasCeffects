#pragma once

/**
 * @file EffectMath.hpp
 * @brief Pure effect simulation: colours, ring/trail/burst geometry and the
 *        trigger detectors.
 *
 * Nothing here touches Android, GLES or the preloader, so every rule that can
 * be wrong without a device - the smash detector, the teleport detector, the
 * fade curve, the ring tessellation - is unit-testable on the build host. The
 * renderer only projects and uploads what this produces.
 *
 * @note All coordinates are Bedrock world space, Y up, in blocks.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace masc {

/** @brief A packed `0xAARRGGBB` colour, the form `DrawCommand::color` uses. */
using Rgba = std::uint32_t;

/**
 * @brief Tessellation and emission clamps.
 *
 * These live with the geometry because they bound what the geometry builders
 * will allocate: a wild config value must not be able to ask for a million
 * vertices on the render thread.
 */
inline constexpr int kMinRingSegments = 8;
inline constexpr int kMaxRingSegments = 96;
inline constexpr int kMinBurstParticles = 4;
inline constexpr int kMaxBurstParticles = 200;

/**
 * @brief Parses `#RRGGBB` or `#AARRGGBB` (also without the `#`).
 *
 * A value that does not parse returns @p fallback, so a malformed config colour
 * can never become a random on-screen colour.
 */
Rgba ParseColor(std::string_view text, Rgba fallback);

/** @brief Multiplies a colour's alpha by @p factor (clamped to 0..1). */
Rgba ScaleAlpha(Rgba color, float factor);

/** @brief Linear blend from @p a to @p b by @p t (clamped to 0..1). */
Rgba MixColor(Rgba a, Rgba b, float t);

// ---------------------------------------------------------------------------
// Trigger detectors
// ---------------------------------------------------------------------------

/** @brief A per-frame sample of the local player's position. */
struct PlayerMotionSample {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  /// Monotonic timestamp of the sample, milliseconds.
  std::int64_t timeMs = 0;
};

/** @brief The outcome of one sample. */
struct MotionEvent {
  /// A smash landing happened at (x, y, z).
  bool smash = false;
  float smashX = 0.0F;
  float smashY = 0.0F;
  float smashZ = 0.0F;

  /// A sudden displacement happened, from the previous point to the current one.
  bool teleport = false;
  float fromX = 0.0F;
  float fromY = 0.0F;
  float fromZ = 0.0F;
  float toX = 0.0F;
  float toY = 0.0F;
  float toZ = 0.0F;
  /// The horizontal distance of that displacement, in blocks.
  float jumpDistance = 0.0F;
};

/**
 * @brief Detects smash landings and teleports across consecutive samples.
 *
 * This is the port's stand-in for the Java mod's impact hook. The preloader
 * publishes the local player's position but not its velocity or ground state,
 * so both events are derived from consecutive positions:
 *
 * <ul>
 *   <li>A <b>smash</b> is a fast descent that stops. The detector keeps the
 *       previous frame's vertical speed and fires when it was at or past
 *       @p minFallSpeed downward and the current frame has arrested that
 *       descent. That is the observable signature of a mace landing, and it
 *       needs no ground flag.</li>
 *   <li>A <b>teleport</b> is a single-frame horizontal displacement of at least
 *       @p minJump blocks. A gap larger than @p maxFrameGapMs is not a
 *       teleport - that is a pause, a world load, or a lag spike - and only
 *       resets the baseline.</li>
 * </ul>
 *
 * <p>Both are client-side inferences from the local player's own motion. The
 * mod cannot see another player's landing, and it does not claim to.
 */
class MotionTracker {
public:
  /** @brief Drops all history, so the next sample cannot fire an event. */
  void reset();

  /**
   * @brief Feeds one sample and reports what it triggered.
   *
   * @param sample        the current frame's player position and timestamp
   * @param minFallSpeed  blocks/second of descent that count as a smash
   * @param minJump       blocks of displacement that count as a teleport
   * @param maxFrameGapMs a gap larger than this is not a teleport
   */
  MotionEvent update(const PlayerMotionSample &sample, double minFallSpeed,
                     double minJump, std::int64_t maxFrameGapMs = 250);

private:
  bool hasPrevious_ = false;
  PlayerMotionSample previous_{};
  double previousVerticalSpeed_ = 0.0;
};

// ---------------------------------------------------------------------------
// Effect geometry
// ---------------------------------------------------------------------------

/** @brief A world-space point. */
struct Point {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

/** @brief A world-space line segment, the primitive every effect is built from. */
struct Segment {
  float x0 = 0.0F;
  float y0 = 0.0F;
  float z0 = 0.0F;
  float x1 = 0.0F;
  float y1 = 0.0F;
  float z1 = 0.0F;
};

/** @brief The kind of entity a hitbox is being drawn for. */
enum class EntityKind {
  /// Another player, or the local player.
  Player,
  /// Any other living entity.
  Mob,
};

/**
 * @brief Tessellates a horizontal shockwave ring.
 *
 * The ring expands from the impact point and fades as it grows, so the caller
 * passes the normalised @p progress (0 at spawn, 1 at death) and receives both
 * the segments and the colour already faded for that instant.
 *
 * @param segments   vertices around the ring (clamped to a sane minimum)
 * @param radius     the ring's current radius in blocks
 * @param thickness  the vertical band height in blocks
 */
std::vector<Segment> BuildShockwave(float centerX, float centerY, float centerZ,
                                    int segments, double radius,
                                    double thickness);

/**
 * @brief Expands a stored trail into ribbon segments.
 *
 * Consecutive points are joined by a segment at @p width above the point, which
 * reads as a ribbon rather than a bare line.
 */
std::vector<Segment> BuildTrail(const std::vector<Point> &points, double width);

/**
 * @brief A deterministic spherical burst.
 *
 * @param count   particles to emit (clamped to a sane range)
 * @param radius  burst radius in blocks
 * @param seed    any value; the same seed always yields the same burst
 */
std::vector<Segment> BuildBurst(float centerX, float centerY, float centerZ,
                                int count, double radius, std::uint32_t seed);

/**
 * @brief A wireframe octahedron, used as the floating death marker.
 *
 * The Java mod draws a sprite skull that rises and fades. Bedrock's preloader
 * exposes no particle or sprite path, so the marker is drawn as world-space
 * geometry instead: an octahedron reads as a distinct floating shape rather
 * than another burst, which is what keeps the death effect recognisable.
 */
std::vector<Segment> BuildDeathMarker(float centerX, float centerY, float centerZ,
                                      double size);

/**
 * @brief The alpha multiplier for a hitbox at @p distance from the player.
 *
 * Matches the Java mod's fade: full strength inside @p fadeDistance, then
 * ramping linearly to zero over the next @p fadeRange blocks. A negative range
 * is treated as zero, so the hitbox simply cuts off at the fade distance rather
 * than inverting.
 */
float HitboxAlpha(float distance, double fadeDistance, double fadeRange);

/** @brief The four corners of an axis-aligned box, as a 12-segment wireframe. */
std::vector<Segment> BuildBoxWireframe(float minX, float minY, float minZ,
                                       float maxX, float maxY, float maxZ);

/** @brief A single segment from the box's top centre along a yaw/pitch heading. */
Segment BuildLookLine(float centerX, float centerY, float centerZ, float yawDeg,
                      float pitchDeg, double length);

/**
 * @brief The eased, decaying alpha multiplier for an effect at @p progress.
 *
 * Rises quickly then decays, so an effect pops and then fades instead of
 * appearing at full strength and vanishing.
 */
float EffectFade(float progress);

} // namespace masc
