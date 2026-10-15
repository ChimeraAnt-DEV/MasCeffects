#include "EffectEngine.hpp"

#include <algorithm>

namespace masc {
namespace {

/// A colour that could not be parsed falls back to a visible default rather than
/// to transparent, so a typo cannot silently disable an effect.
Rgba ColorOr(std::string_view text, Rgba fallback) {
  return ParseColor(text, fallback);
}

} // namespace

ConfigSnapshot ConfigSnapshot::From(const MasEffectConfig &config) {
  ConfigSnapshot snapshot;
  snapshot.shockwaveEnabled = config.shockwaveEnabled;
  snapshot.shockwaveSegments = config.shockwaveSegments;
  snapshot.shockwaveRadius = config.shockwaveRadius;
  snapshot.shockwaveLifeMs = config.shockwaveLifeMs;
  snapshot.shockwaveThickness = config.shockwaveThickness;
  snapshot.shockwaveColor = ColorOr(config.shockwaveColor, 0xFF7FD4FFu);
  snapshot.shockwaveAutoOnLanding = config.shockwaveAutoOnLanding;
  snapshot.shockwaveMinFallSpeed = config.shockwaveMinFallSpeed;

  snapshot.pearlTrailEnabled = config.pearlTrailEnabled;
  snapshot.pearlTrailColor = ColorOr(config.pearlTrailColor, 0xFFC08CFFu);
  snapshot.pearlTrailLifeMs = config.pearlTrailLifeMs;
  snapshot.pearlTrailWidth = config.pearlTrailWidth;
  snapshot.pearlTrailMinJump = config.pearlTrailMinJump;

  snapshot.impactBurstEnabled = config.impactBurstEnabled;
  snapshot.impactBurstColor = ColorOr(config.impactBurstColor, 0xFFFF4D4Du);
  snapshot.impactBurstLifeMs = config.impactBurstLifeMs;
  snapshot.impactBurstParticles = config.impactBurstParticles;
  snapshot.impactBurstRadius = config.impactBurstRadius;

  snapshot.deathEffectEnabled = config.deathEffectEnabled;
  snapshot.deathEffectColor = ColorOr(config.deathEffectColor, 0xFFFF4D4Du);
  snapshot.deathEffectLifeMs = config.deathEffectLifeMs;
  snapshot.deathEffectParticles = config.deathEffectParticles;
  snapshot.deathEffectRadius = config.deathEffectRadius;
  snapshot.deathEffectMarkerSize = config.deathEffectMarkerSize;
  snapshot.deathEffectRise = config.deathEffectRise;
  snapshot.deathEffectMaxHealth = config.deathEffectMaxHealth;
  snapshot.deathEffectMaxConcurrent = config.deathEffectMaxConcurrent;

  snapshot.hitboxEnabled = config.hitboxEnabled;
  snapshot.hitboxOpacity = config.hitboxOpacity;
  snapshot.hitboxColor = ColorOr(config.hitboxColor, 0xFFFFFFFFu);
  snapshot.hitboxMobOpacity = config.hitboxMobOpacity;
  snapshot.hitboxSelfOpacity = config.hitboxSelfOpacity;
  snapshot.hitboxShowLookLine = config.hitboxShowLookLine;
  snapshot.hitboxFadeDistance = config.hitboxFadeDistance;
  snapshot.hitboxFadeRange = config.hitboxFadeRange;
  snapshot.hitboxMaxEntities = config.hitboxMaxEntities;

  snapshot.showStatusHud = config.showStatusHud;
  snapshot.statusHudScale = config.statusHudScale;
  snapshot.showDiagnostics = config.showDiagnostics;
  return snapshot;
}

void EffectEngine::reset() {
  effects_.clear();
  trail_.clear();
  health_.clear();
  tracker_.reset();
  hasTick_ = false;
  lastTick_ = 0;
  lastUpdateMs_ = 0;
}

void EffectEngine::dropExpired(std::int64_t nowMs) {
  effects_.erase(
      std::remove_if(effects_.begin(), effects_.end(),
                     [nowMs](const Effect &effect) {
                       return nowMs - effect.bornMs >=
                              static_cast<std::int64_t>(effect.lifeMs);
                     }),
      effects_.end());
}

void EffectEngine::spawnSmash(float x, float y, float z,
                             const ConfigSnapshot &config,
                             std::int64_t nowMs) {
  if (!config.shockwaveEnabled) return;
  if (effects_.size() >= kMaxEffects) effects_.erase(effects_.begin());

  Effect effect;
  effect.kind = EffectKind::Shockwave;
  effect.x = x;
  effect.y = y;
  effect.z = z;
  effect.bornMs = nowMs;
  effect.lifeMs = config.shockwaveLifeMs;
  effect.color = config.shockwaveColor;
  effect.radius = config.shockwaveRadius;
  effect.segments = config.shockwaveSegments;
  effect.thickness = config.shockwaveThickness;
  effects_.push_back(effect);
}

void EffectEngine::spawnTeleport(float fromX, float fromY, float fromZ,
                                float toX, float toY, float toZ,
                                const ConfigSnapshot &config,
                                std::int64_t nowMs) {
  if (config.impactBurstEnabled) {
    if (effects_.size() >= kMaxEffects) effects_.erase(effects_.begin());
    Effect burst;
    burst.kind = EffectKind::ImpactBurst;
    burst.x = fromX;
    burst.y = fromY;
    burst.z = fromZ;
    burst.bornMs = nowMs;
    burst.lifeMs = config.impactBurstLifeMs;
    burst.color = config.impactBurstColor;
    burst.radius = config.impactBurstRadius;
    burst.particles = config.impactBurstParticles;
    burst.seed = seed_ = seed_ * 1664525u + 1013904223u;
    effects_.push_back(burst);
  }

  // The trail spans the jump, so it reads as a thrown-pearl arc rather than a
  // line that appears only at the destination.
  trail_.clear();
  trail_.push_back(TrailPoint{{fromX, fromY, fromZ}, nowMs});
  trail_.push_back(TrailPoint{{toX, toY, toZ}, nowMs});
}

void EffectEngine::update(std::uint64_t tick, std::int64_t nowMs, float playerX,
                          float playerY, float playerZ,
                          const ConfigSnapshot &config) {
  // The render callback runs once per rendered player model, so the same tick
  // arrives repeatedly. Only the first call for a tick advances the simulation.
  if (hasTick_ && tick == lastTick_) return;
  hasTick_ = true;
  lastTick_ = tick;
  lastUpdateMs_ = nowMs;

  dropExpired(nowMs);

  // Age the trail so an idle player does not keep a stale ribbon on screen.
  if (!trail_.empty() &&
      nowMs - trail_.back().stampMs >
          static_cast<std::int64_t>(config.pearlTrailLifeMs)) {
    trail_.clear();
  }

  const MotionEvent event = tracker_.update(
      PlayerMotionSample{playerX, playerY, playerZ, nowMs},
      config.shockwaveMinFallSpeed, config.pearlTrailMinJump);

  if (event.smash) {
    ++smashCount_;
    if (config.shockwaveAutoOnLanding) {
      spawnSmash(event.smashX, event.smashY, event.smashZ, config, nowMs);
    }
  }
  if (event.teleport) {
    ++teleportCount_;
    if (config.pearlTrailEnabled) {
      spawnTeleport(event.fromX, event.fromY, event.fromZ, event.toX,
                    event.toY, event.toZ, config, nowMs);
    }
  }
}

void EffectEngine::spawnDeath(float x, float y, float z,
                             const ConfigSnapshot &config, std::int64_t nowMs) {
  if (!config.deathEffectEnabled) return;

  // Death effects are capped separately from the other effects, so a busy fight
  // cannot push the shockwave out of the list.
  std::size_t deaths = 0;
  for (const Effect &effect : effects_) {
    if (effect.kind == EffectKind::Death) ++deaths;
  }
  if (deaths >= static_cast<std::size_t>(
                    std::max(config.deathEffectMaxConcurrent, 1))) {
    for (auto it = effects_.begin(); it != effects_.end(); ++it) {
      if (it->kind == EffectKind::Death) {
        effects_.erase(it);
        break;
      }
    }
  }
  if (effects_.size() >= kMaxEffects) effects_.erase(effects_.begin());

  Effect effect;
  effect.kind = EffectKind::Death;
  effect.x = x;
  effect.y = y;
  effect.z = z;
  effect.bornMs = nowMs;
  effect.lifeMs = config.deathEffectLifeMs;
  effect.color = config.deathEffectColor;
  effect.radius = config.deathEffectRadius;
  effect.particles = config.deathEffectParticles;
  effect.markerSize = config.deathEffectMarkerSize;
  effect.rise = config.deathEffectRise;
  effect.seed = seed_ = seed_ * 1664525u + 1013904223u;
  effects_.push_back(effect);
}

void EffectEngine::observeEntities(const std::vector<GameEntity> &entities,
                                   const ConfigSnapshot &config,
                                   std::int64_t nowMs) {
  // Only track while the death effect is on, so a disabled module does no work.
  if (!config.deathEffectEnabled) {
    health_.clear();
    return;
  }

  const std::int64_t debounceMs = 5000;
  for (const GameEntity &entity : entities) {
    // An unreadable health reading can never fire a death, so a wrong health
    // offset produces no effect instead of a false one.
    if (entity.health < 0.0F) continue;
    // Skip the local player: its own death already shows the death screen, and
    // the Java mod's effect is for the players around you.
    if (entity.isLocalPlayer) continue;

    HealthTrack *track = nullptr;
    for (HealthTrack &candidate : health_) {
      if (candidate.entity == entity.entityId) {
        track = &candidate;
        break;
      }
    }
    if (track == nullptr) {
      // Bound the table so a long session cannot grow it without limit.
      if (health_.size() >= kMaxTrackedEntities) health_.erase(health_.begin());
      health_.push_back(HealthTrack{entity.entityId, false, 0, false});
      track = &health_.back();
    }

    if (entity.health > 0.0F && entity.health <= config.deathEffectMaxHealth) {
      track->sawLiveHealth = true;
      track->firedDeath = false;
      continue;
    }

    // Health at (or below) zero. Only a drop from a plausible live reading is a
    // death; a source that only ever reports zero never fires.
    if (!track->sawLiveHealth) continue;
    if (track->firedDeath && nowMs - track->lastDeathMs < debounceMs) continue;

    track->lastDeathMs = nowMs;
    track->firedDeath = true;
    track->sawLiveHealth = false;
    ++deathCount_;
    spawnDeath(entity.x, entity.y, entity.z, config, nowMs);
  }
}

void EffectEngine::requestShockwave(float x, float y, float z,
                                   const ConfigSnapshot &config,
                                   std::int64_t nowMs) {
  spawnSmash(x, y, z, config, nowMs);
}

std::vector<DrawBatch> EffectEngine::collect(std::int64_t nowMs,
                                             const ConfigSnapshot &config) {
  std::vector<DrawBatch> batches;

  for (const Effect &effect : effects_) {
    const double elapsed = static_cast<double>(nowMs - effect.bornMs);
    if (elapsed < 0.0 || elapsed >= effect.lifeMs) continue;
    const auto progress =
        static_cast<float>(elapsed / std::max(effect.lifeMs, 1.0));
    const float fade = EffectFade(progress);

    DrawBatch batch;
    switch (effect.kind) {
      case EffectKind::Shockwave: {
        // The ring expands as it fades, which is what makes it read as a shell
        // travelling outward from the impact.
        const double radius = effect.radius * static_cast<double>(progress);
        batch.segments = BuildShockwave(effect.x, effect.y, effect.z,
                                        effect.segments, radius,
                                        effect.thickness);
        batch.color = ScaleAlpha(effect.color, fade);
        break;
      }
      case EffectKind::ImpactBurst: {
        batch.segments = BuildBurst(effect.x, effect.y, effect.z,
                                    effect.particles, effect.radius, effect.seed);
        batch.color = ScaleAlpha(effect.color, fade);
        break;
      }
      case EffectKind::Death: {
        // The Java mod's death effect is a skull that rises and fades. With no
        // sprite path on this platform, the burst plus a rising octahedron
        // marker keeps the effect recognisable as a death rather than another
        // generic burst.
        batch.segments = BuildBurst(effect.x, effect.y, effect.z,
                                    effect.particles, effect.radius, effect.seed);
        const float risen =
            effect.y + static_cast<float>(effect.rise * progress);
        const std::vector<Segment> marker =
            BuildDeathMarker(effect.x, risen, effect.z, effect.markerSize);
        batch.segments.insert(batch.segments.end(), marker.begin(), marker.end());
        batch.color = ScaleAlpha(effect.color, fade);
        break;
      }
    }
    if (!batch.segments.empty()) batches.push_back(std::move(batch));
  }

  if (config.pearlTrailEnabled && trail_.size() >= 2) {
    std::vector<Point> points;
    points.reserve(trail_.size());
    for (const TrailPoint &entry : trail_) {
      points.push_back(entry.point);
    }
    DrawBatch batch;
    batch.segments = BuildTrail(points, config.pearlTrailWidth);
    // The trail's own age drives its fade, so it dims while it lingers.
    const double age =
        static_cast<double>(nowMs - trail_.back().stampMs);
    const auto progress = static_cast<float>(
        std::clamp(age / std::max(config.pearlTrailLifeMs, 1.0), 0.0, 1.0));
    batch.color = ScaleAlpha(config.pearlTrailColor, 1.0F - progress);
    if (!batch.segments.empty()) batches.push_back(std::move(batch));
  }

  return batches;
}

} // namespace masc
