// Host unit tests for the pure effect maths. No Android, no GLES, no preloader,
// so every rule that can be wrong without a device is checked here.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "EffectMath.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  std::printf("FAIL: %s\n", what.c_str());
}

void CheckNear(double actual, double expected, double tolerance,
               const std::string &what) {
  ++g_checks;
  if (std::fabs(actual - expected) <= tolerance) return;
  ++g_failures;
  std::printf("FAIL: %s (expected %.4f, got %.4f)\n", what.c_str(), expected,
              actual);
}

masc::PlayerMotionSample Sample(float x, float y, float z, std::int64_t ms) {
  return masc::PlayerMotionSample{x, y, z, ms};
}

// ---------------------------------------------------------------------------

void TestColorParsing() {
  Check(masc::ParseColor("#FF0000", 0) == 0xFFFF0000u, "6-digit is opaque red");
  Check(masc::ParseColor("FF0000", 0) == 0xFFFF0000u, "leading # is optional");
  Check(masc::ParseColor("#80FF0000", 0) == 0x80FF0000u, "8-digit keeps alpha");
  Check(masc::ParseColor("", 0xDEADBEEFu) == 0xDEADBEEFu, "empty uses fallback");
  Check(masc::ParseColor("#GG0000", 0x12345678u) == 0x12345678u,
        "non-hex uses fallback");
  Check(masc::ParseColor("#FFF", 0x12345678u) == 0x12345678u,
        "wrong length uses fallback");
}

void TestAlphaAndMix() {
  Check(masc::ScaleAlpha(0xFFFFFFFFu, 0.5F) == 0x80FFFFFFu,
        "half alpha halves the high byte");
  Check(masc::ScaleAlpha(0xFFFFFFFFu, 2.0F) == 0xFFFFFFFFu, "alpha clamps at 255");
  Check(masc::ScaleAlpha(0xFFFFFFFFu, -1.0F) == 0x00FFFFFFu, "alpha clamps at 0");

  Check(masc::MixColor(0xFF000000u, 0xFFFFFFFFu, 0.0F) == 0xFF000000u,
        "mix at 0 is the first colour");
  Check(masc::MixColor(0xFF000000u, 0xFFFFFFFFu, 1.0F) == 0xFFFFFFFFu,
        "mix at 1 is the second colour");
  // 0x00 -> 0xFF at t = 0.5 is 127.5, which rounds away from zero to 0x80.
  Check(masc::MixColor(0xFF000000u, 0xFFFFFFFFu, 0.5F) == 0xFF808080u,
        "mix at 0.5 is the rounded midpoint");
}

void TestSmashDetection() {
  masc::MotionTracker tracker;

  // A normal walk must never fire.
  tracker.update(Sample(0, 64, 0, 0), 14.0, 4.0);
  const masc::MotionEvent walking = tracker.update(Sample(0.2F, 64, 0, 50), 14.0, 4.0);
  Check(!walking.smash, "walking is not a smash");
  Check(!walking.teleport, "walking is not a teleport");

  // A fast descent that stops is a smash. 2 blocks in 50 ms is 40 blocks/s.
  masc::MotionTracker falling;
  falling.update(Sample(0, 70, 0, 0), 14.0, 4.0);
  falling.update(Sample(0, 68, 0, 50), 14.0, 4.0);
  const masc::MotionEvent landing =
      falling.update(Sample(0, 67.95F, 0, 100), 14.0, 4.0);
  Check(landing.smash, "fast descent that stops is a smash");
  CheckNear(landing.smashY, 67.95, 0.001, "smash reports the landing point");

  // A slow descent must not fire, so a gentle drop is not a smash.
  masc::MotionTracker slow;
  slow.update(Sample(0, 70, 0, 0), 14.0, 4.0);
  slow.update(Sample(0, 69.9F, 0, 50), 14.0, 4.0);
  const masc::MotionEvent gentle = slow.update(Sample(0, 69.85F, 0, 100), 14.0, 4.0);
  Check(!gentle.smash, "a slow descent is not a smash");
}

void TestSmashNeedsAnArrest() {
  // Still falling fast at the second sample: no arrest, so no smash yet.
  masc::MotionTracker tracker;
  tracker.update(Sample(0, 70, 0, 0), 14.0, 4.0);
  tracker.update(Sample(0, 68, 0, 50), 14.0, 4.0);
  const masc::MotionEvent stillFalling =
      tracker.update(Sample(0, 66, 0, 100), 14.0, 4.0);
  Check(!stillFalling.smash, "a continuing fall does not fire a smash");

  // The very next sample arrests the descent, and that is the smash.
  const masc::MotionEvent arrested =
      tracker.update(Sample(0, 65.9F, 0, 150), 14.0, 4.0);
  Check(arrested.smash, "the arrest after a fall is the smash");
}

void TestTeleportDetection() {
  masc::MotionTracker tracker;
  tracker.update(Sample(0, 64, 0, 0), 14.0, 4.0);
  const masc::MotionEvent jump =
      tracker.update(Sample(20, 68, 0, 50), 14.0, 4.0);
  Check(jump.teleport, "a 20-block jump is a teleport");
  CheckNear(jump.fromX, 0.0, 0.001, "teleport reports the origin");
  CheckNear(jump.toX, 20.0, 0.001, "teleport reports the destination");
  CheckNear(jump.jumpDistance, 20.0, 0.001, "teleport reports the distance");

  // A small step is not a teleport.
  masc::MotionTracker small;
  small.update(Sample(0, 64, 0, 0), 14.0, 4.0);
  Check(!small.update(Sample(0.5F, 64, 0, 50), 14.0, 4.0).teleport,
        "a half-block step is not a teleport");
}

void TestTeleportIgnoresTimeGaps() {
  // A long gap is a pause or a world load, not a teleport, even though the
  // displacement is large. Firing here would spawn effects on every reconnect.
  masc::MotionTracker tracker;
  tracker.update(Sample(0, 64, 0, 0), 14.0, 4.0);
  const masc::MotionEvent afterGap =
      tracker.update(Sample(500, 64, 0, 5000), 14.0, 4.0);
  Check(!afterGap.teleport, "a large displacement after a long gap is not a teleport");
}

void TestShockwaveTessellation() {
  const auto ring = masc::BuildShockwave(0, 64, 0, 32, 5.0, 0.1);
  Check(ring.size() == 32, "the ring has one segment per vertex");

  // Every vertex must sit on the requested radius, and inside the band.
  for (const masc::Segment &segment : ring) {
    const double radius = std::sqrt(segment.x0 * segment.x0 +
                                    segment.z0 * segment.z0);
    CheckNear(radius, 5.0, 0.001, "each ring vertex is on the radius");
    CheckNear(segment.y0, 63.95, 0.001, "each ring vertex is on the low band");
    CheckNear(segment.y1, 64.05, 0.001, "each ring vertex is on the high band");
  }

  // The segment count is clamped, so a wild config cannot allocate unboundedly.
  Check(masc::BuildShockwave(0, 0, 0, 1, 1.0, 0.1).size() ==
            static_cast<std::size_t>(masc::kMinRingSegments),
        "the segment count clamps up");
  Check(masc::BuildShockwave(0, 0, 0, 100000, 1.0, 0.1).size() ==
            static_cast<std::size_t>(masc::kMaxRingSegments),
        "the segment count clamps down");
}

void TestTrail() {
  const std::vector<masc::Point> points = {{0, 64, 0}, {1, 65, 1}, {2, 66, 2}};
  const auto trail = masc::BuildTrail(points, 0.2);
  Check(trail.size() == 2, "a 3-point trail yields 2 segments");
  CheckNear(trail[0].y0, 64.2, 0.001, "the ribbon is offset upward");
  CheckNear(trail[1].y1, 66.2, 0.001, "the last point is offset too");

  Check(masc::BuildTrail({{0, 0, 0}}, 0.2).empty(),
        "a single point yields no trail");
  Check(masc::BuildTrail({}, 0.2).empty(), "an empty trail yields nothing");
}

void TestBurstIsDeterministicAndBounded() {
  const auto first = masc::BuildBurst(0, 64, 0, 24, 1.5, 12345u);
  const auto second = masc::BuildBurst(0, 64, 0, 24, 1.5, 12345u);
  Check(first.size() == second.size(), "the same seed yields the same count");
  for (std::size_t i = 0; i < first.size(); ++i) {
    CheckNear(first[i].x1, second[i].x1, 1e-6, "the same seed is reproducible");
  }

  // A different seed must actually differ, or the burst is not really seeded.
  const auto other = masc::BuildBurst(0, 64, 0, 24, 1.5, 999u);
  bool differs = false;
  for (std::size_t i = 0; i < other.size(); ++i) {
    if (std::fabs(other[i].x1 - first[i].x1) > 1e-6) differs = true;
  }
  Check(differs, "a different seed yields a different burst");

  // Every particle must be exactly on the burst radius.
  for (const masc::Segment &segment : first) {
    const double dx = segment.x1 - segment.x0;
    const double dy = segment.y1 - segment.y0;
    const double dz = segment.z1 - segment.z0;
    CheckNear(std::sqrt(dx * dx + dy * dy + dz * dz), 1.5, 0.001,
              "each particle is on the burst radius");
  }

  Check(masc::BuildBurst(0, 0, 0, 1, 1.0, 7u).size() ==
            static_cast<std::size_t>(masc::kMinBurstParticles),
        "the particle count clamps up");
  Check(masc::BuildBurst(0, 0, 0, 100000, 1.0, 7u).size() ==
            static_cast<std::size_t>(masc::kMaxBurstParticles),
        "the particle count clamps down");
}

void TestBoxWireframe() {
  const auto box = masc::BuildBoxWireframe(0, 64, 0, 0.6F, 65.8F, 0.6F);
  Check(box.size() == 12, "a box is 12 edges");

  // The bounds must be respected exactly, with nothing extrapolated.
  float minX = 1e9F, maxX = -1e9F, minY = 1e9F, maxY = -1e9F;
  for (const masc::Segment &edge : box) {
    for (float x : {edge.x0, edge.x1}) {
      minX = std::fmin(minX, x);
      maxX = std::fmax(maxX, x);
    }
    for (float y : {edge.y0, edge.y1}) {
      minY = std::fmin(minY, y);
      maxY = std::fmax(maxY, y);
    }
  }
  CheckNear(minX, 0.0, 0.001, "the box respects its minimum X");
  CheckNear(maxX, 0.6, 0.001, "the box respects its maximum X");
  CheckNear(minY, 64.0, 0.001, "the box respects its minimum Y");
  CheckNear(maxY, 65.8, 0.001, "the box respects its maximum Y");
}

void TestLookLine() {
  // Yaw 0 with pitch 0 must point along +Z, which is Bedrock's forward.
  const masc::Segment forward = masc::BuildLookLine(0, 64, 0, 0, 0, 2.0);
  CheckNear(forward.z1, 2.0, 0.001, "yaw 0 looks toward +Z");
  CheckNear(forward.x1, 0.0, 0.001, "yaw 0 has no X component");

  // Looking straight down must move the endpoint below the origin.
  const masc::Segment down = masc::BuildLookLine(0, 64, 0, 0, 90, 2.0);
  CheckNear(down.y1, 62.0, 0.001, "pitch 90 looks straight down");
}

void TestEffectFade() {
  CheckNear(masc::EffectFade(0.0F), 0.0, 0.001, "an effect starts invisible");
  CheckNear(masc::EffectFade(0.15F), 1.0, 0.001, "an effect peaks quickly");
  CheckNear(masc::EffectFade(1.0F), 0.0, 0.001, "an effect ends invisible");
  Check(masc::EffectFade(0.5F) > 0.0F, "an effect is visible mid-life");
  // Out-of-range progress must clamp rather than extrapolate.
  CheckNear(masc::EffectFade(-1.0F), 0.0, 0.001, "negative progress clamps");
  CheckNear(masc::EffectFade(2.0F), 0.0, 0.001, "progress past 1 clamps");
}

void TestHitboxAlpha() {
  // Inside the fade distance a box is fully solid.
  CheckNear(masc::HitboxAlpha(0.0F, 15.0, 20.0), 1.0, 0.001,
            "a box at zero distance is solid");
  CheckNear(masc::HitboxAlpha(15.0F, 15.0, 20.0), 1.0, 0.001,
            "a box at the fade distance is still solid");
  // Past it, the fade ramps linearly to nothing.
  CheckNear(masc::HitboxAlpha(25.0F, 15.0, 20.0), 0.5, 0.001,
            "a box halfway through the fade is half opacity");
  CheckNear(masc::HitboxAlpha(35.0F, 15.0, 20.0), 0.0, 0.001,
            "a box past the fade is invisible");
  CheckNear(masc::HitboxAlpha(100.0F, 15.0, 20.0), 0.0, 0.001,
            "a far box clamps to invisible");
  // A zero range means the box simply cuts off rather than inverting.
  CheckNear(masc::HitboxAlpha(15.0F, 15.0, 0.0), 1.0, 0.001,
            "a zero fade range is solid at the distance");
  CheckNear(masc::HitboxAlpha(15.1F, 15.0, 0.0), 0.0, 0.001,
            "a zero fade range cuts off past the distance");
}

void TestDeathMarker() {
  const auto marker = masc::BuildDeathMarker(0, 64, 0, 0.5);
  Check(marker.size() == 12, "an octahedron marker is 12 edges");

  // Every vertex must sit on the requested radius from the centre, so the
  // marker is the size the config asked for.
  for (const masc::Segment &edge : marker) {
    const double rx = edge.x0;
    const double ry = edge.y0 - 64.0;
    const double rz = edge.z0;
    CheckNear(std::sqrt(rx * rx + ry * ry + rz * rz), 0.5, 0.001,
              "each marker vertex is on the marker radius");
  }

  // It must be vertically symmetric about the centre it was given.
  float minY = 1e9F, maxY = -1e9F;
  for (const masc::Segment &edge : marker) {
    minY = std::fmin(minY, std::fmin(edge.y0, edge.y1));
    maxY = std::fmax(maxY, std::fmax(edge.y0, edge.y1));
  }
  CheckNear(minY, 63.5, 0.001, "the marker extends below its centre");
  CheckNear(maxY, 64.5, 0.001, "the marker extends above its centre");
}

} // namespace

int main() {
  TestColorParsing();
  TestAlphaAndMix();
  TestSmashDetection();
  TestSmashNeedsAnArrest();
  TestTeleportDetection();
  TestTeleportIgnoresTimeGaps();
  TestShockwaveTessellation();
  TestTrail();
  TestBurstIsDeterministicAndBounded();
  TestBoxWireframe();
  TestLookLine();
  TestEffectFade();
  TestHitboxAlpha();
  TestDeathMarker();

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
