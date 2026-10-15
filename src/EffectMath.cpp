#include "EffectMath.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace masc {
namespace {

int ClampInt(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

float Clamp01(float value) {
  return value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
}

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

/// xorshift32: a small deterministic stream so a burst is reproducible.
std::uint32_t NextRandom(std::uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

/// A deterministic value in [-1, 1].
float SignedUnit(std::uint32_t &state) {
  const float unit =
      static_cast<float>(NextRandom(state) & 0xFFFFFFu) / 16777215.0F;
  return unit * 2.0F - 1.0F;
}

} // namespace

Rgba ParseColor(std::string_view text, Rgba fallback) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);
  if (text.size() != 6 && text.size() != 8) return fallback;
  for (char c : text) {
    if (!IsHexDigit(c)) return fallback;
  }

  std::uint32_t value = 0;
  for (char c : text) {
    value = (value << 4) | static_cast<std::uint32_t>(HexValue(c));
  }
  // A 6-digit value has no alpha component, so it is treated as opaque.
  return text.size() == 6 ? (value | 0xFF000000u) : value;
}

Rgba ScaleAlpha(Rgba color, float factor) {
  const float alpha = static_cast<float>((color >> 24) & 0xFFu) * factor;
  const auto scaled = static_cast<std::uint32_t>(
      std::lround(std::clamp(alpha, 0.0F, 255.0F)));
  return (color & 0x00FFFFFFu) | (scaled << 24);
}

Rgba MixColor(Rgba a, Rgba b, float t) {
  const float k = Clamp01(t);
  auto channel = [k](std::uint32_t lo, std::uint32_t hi) {
    const float blended =
        static_cast<float>(lo) + (static_cast<float>(hi) - static_cast<float>(lo)) * k;
    return static_cast<std::uint32_t>(std::lround(blended));
  };
  const std::uint32_t alpha = channel((a >> 24) & 0xFFu, (b >> 24) & 0xFFu);
  const std::uint32_t red = channel((a >> 16) & 0xFFu, (b >> 16) & 0xFFu);
  const std::uint32_t green = channel((a >> 8) & 0xFFu, (b >> 8) & 0xFFu);
  const std::uint32_t blue = channel(a & 0xFFu, b & 0xFFu);
  return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

// ---------------------------------------------------------------------------
// MotionTracker
// ---------------------------------------------------------------------------

void MotionTracker::reset() {
  hasPrevious_ = false;
  previous_ = PlayerMotionSample{};
  previousVerticalSpeed_ = 0.0;
}

MotionEvent MotionTracker::update(const PlayerMotionSample &sample,
                                 double minFallSpeed, double minJump,
                                 std::int64_t maxFrameGapMs) {
  MotionEvent event{};
  if (!hasPrevious_) {
    previous_ = sample;
    previousVerticalSpeed_ = 0.0;
    hasPrevious_ = true;
    return event;
  }

  const double dx = static_cast<double>(sample.x) - previous_.x;
  const double dy = static_cast<double>(sample.y) - previous_.y;
  const double dz = static_cast<double>(sample.z) - previous_.z;
  const double horizontal = std::sqrt(dx * dx + dz * dz);
  const std::int64_t gap = sample.timeMs - previous_.timeMs;

  // A pause, a world load or a lag spike makes the next delta meaningless, so
  // the frame only re-establishes the baseline. Firing here would spawn a
  // shockwave every time the player reconnected.
  if (gap <= 0 || gap > maxFrameGapMs) {
    previous_ = sample;
    previousVerticalSpeed_ = 0.0;
    return event;
  }

  const double seconds = static_cast<double>(gap) / 1000.0;
  const double verticalSpeed = dy / seconds;

  // A smash is a fast descent that has just been arrested. The descent is read
  // from the *previous* interval because the interval that contains the landing
  // is short and already slowed by it.
  if (previousVerticalSpeed_ <= -minFallSpeed && verticalSpeed > -minFallSpeed) {
    event.smash = true;
    event.smashX = sample.x;
    event.smashY = sample.y;
    event.smashZ = sample.z;
  }

  // A teleport must be sudden and large; a sprint or a knockback is neither.
  if (horizontal >= minJump) {
    event.teleport = true;
    event.fromX = previous_.x;
    event.fromY = previous_.y;
    event.fromZ = previous_.z;
    event.toX = sample.x;
    event.toY = sample.y;
    event.toZ = sample.z;
    event.jumpDistance = static_cast<float>(horizontal);
  }

  previous_ = sample;
  previousVerticalSpeed_ = verticalSpeed;
  return event;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

std::vector<Segment> BuildShockwave(float centerX, float centerY, float centerZ,
                                   int segments, double radius,
                                   double thickness) {
  const int count =
      ClampInt(segments, kMinRingSegments, kMaxRingSegments);
  const float r = static_cast<float>(std::max(radius, 0.0));
  const float halfBand = static_cast<float>(std::max(thickness, 0.0)) * 0.5F;
  const float lowY = centerY - halfBand;
  const float highY = centerY + halfBand;

  std::vector<Segment> out;
  out.reserve(static_cast<std::size_t>(count));
  const float step = static_cast<float>(2.0 * std::numbers::pi) /
                     static_cast<float>(count);
  for (int i = 0; i < count; ++i) {
    const float a0 = step * static_cast<float>(i);
    const float a1 = step * static_cast<float>(i + 1);
    // Each vertex is a short vertical wall at the ring's radius. Drawn as lines
    // these read as the expanding shell of the smash.
    out.push_back(Segment{
        centerX + std::cos(a0) * r, lowY, centerZ + std::sin(a0) * r,
        centerX + std::cos(a1) * r, highY, centerZ + std::sin(a1) * r,
    });
  }
  return out;
}

std::vector<Segment> BuildTrail(const std::vector<Point> &points, double width) {
  std::vector<Segment> out;
  if (points.size() < 2) return out;

  const float w = static_cast<float>(std::max(width, 0.0));
  out.reserve(points.size() - 1);
  for (std::size_t i = 1; i < points.size(); ++i) {
    const Point &a = points[i - 1];
    const Point &b = points[i];
    // A ribbon segment: the two endpoints offset upward, which gives the trail
    // visible thickness from the side without needing a camera-facing quad.
    out.push_back(Segment{a.x, a.y + w, a.z, b.x, b.y + w, b.z});
  }
  return out;
}

std::vector<Segment> BuildBurst(float centerX, float centerY, float centerZ,
                               int count, double radius, std::uint32_t seed) {
  const int n = ClampInt(count, kMinBurstParticles, kMaxBurstParticles);
  const float r = static_cast<float>(std::max(radius, 0.0));

  std::uint32_t state = seed == 0 ? 1u : seed;
  std::vector<Segment> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float dx = SignedUnit(state);
    const float dy = SignedUnit(state);
    const float dz = SignedUnit(state);
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Skip a degenerate direction rather than dividing by (near) zero.
    if (length < 1.0e-4F) continue;
    const float scale = r / length;
    out.push_back(Segment{centerX, centerY, centerZ, centerX + dx * scale,
                          centerY + dy * scale, centerZ + dz * scale});
  }
  return out;
}

std::vector<Segment> BuildDeathMarker(float centerX, float centerY, float centerZ,
                                     double size) {
  const float s = static_cast<float>(std::max(size, 0.0));
  const Point top{centerX, centerY + s, centerZ};
  const Point bottom{centerX, centerY - s, centerZ};
  const Point equator[4] = {
      {centerX + s, centerY, centerZ},
      {centerX, centerY, centerZ + s},
      {centerX - s, centerY, centerZ},
      {centerX, centerY, centerZ - s},
  };

  std::vector<Segment> out;
  out.reserve(12); // An octahedron has 12 edges.
  for (int i = 0; i < 4; ++i) {
    const Point &a = equator[i];
    const Point &b = equator[(i + 1) % 4];
    out.push_back(Segment{top.x, top.y, top.z, a.x, a.y, a.z});
    out.push_back(Segment{a.x, a.y, a.z, b.x, b.y, b.z});
    out.push_back(Segment{b.x, b.y, b.z, bottom.x, bottom.y, bottom.z});
  }
  return out;
}

float HitboxAlpha(float distance, double fadeDistance, double fadeRange) {
  if (distance <= static_cast<float>(fadeDistance)) return 1.0F;
  const auto range = static_cast<float>(std::max(fadeRange, 0.0));
  if (range <= 0.0F) return 0.0F;
  const float beyond = distance - static_cast<float>(fadeDistance);
  return Clamp01(1.0F - beyond / range);
}

std::vector<Segment> BuildBoxWireframe(float minX, float minY, float minZ,
                                      float maxX, float maxY, float maxZ) {
  const Point corners[8] = {
      {minX, minY, minZ}, {maxX, minY, minZ}, {maxX, minY, maxZ}, {minX, minY, maxZ},
      {minX, maxY, minZ}, {maxX, maxY, minZ}, {maxX, maxY, maxZ}, {minX, maxY, maxZ},
  };
  // Bottom quad, top quad, then the four vertical edges.
  constexpr int kEdges[12][2] = {
      {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
      {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };

  std::vector<Segment> out;
  out.reserve(12);
  for (const auto &edge : kEdges) {
    const Point &a = corners[edge[0]];
    const Point &b = corners[edge[1]];
    out.push_back(Segment{a.x, a.y, a.z, b.x, b.y, b.z});
  }
  return out;
}

Segment BuildLookLine(float centerX, float centerY, float centerZ, float yawDeg,
                      float pitchDeg, double length) {
  // Bedrock yaw is measured clockwise from -Z; pitch is positive downward.
  const float yaw = yawDeg * static_cast<float>(std::numbers::pi) / 180.0F;
  const float pitch = pitchDeg * static_cast<float>(std::numbers::pi) / 180.0F;
  const float horizontal = std::cos(pitch);
  const float dx = -std::sin(yaw) * horizontal;
  const float dy = -std::sin(pitch);
  const float dz = std::cos(yaw) * horizontal;
  const float len = static_cast<float>(std::max(length, 0.0));
  return Segment{centerX, centerY, centerZ, centerX + dx * len,
                 centerY + dy * len, centerZ + dz * len};
}

float EffectFade(float progress) {
  const float p = Clamp01(progress);
  // Attack fast, then decay: a linear fade reads as a flat, lifeless effect.
  constexpr float kAttack = 0.15F;
  if (p < kAttack) return p / kAttack;
  const float decay = (p - kAttack) / (1.0F - kAttack);
  return 1.0F - decay * decay;
}

} // namespace masc
