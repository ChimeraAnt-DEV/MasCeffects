#include "MasCeffectsMod.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <pl/Logger.hpp>
#include <pl/ModMenu.hpp>

namespace masc {
namespace {

std::int64_t MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 +
         static_cast<std::int64_t>(ts.tv_nsec / 1000000);
}

/// Formats a double without trailing noise, for a Mod Menu default value.
std::string ToMenuValue(double value) {
  std::string text = std::to_string(value);
  // Trim trailing zeros so the menu shows "6" rather than "6.000000".
  if (text.find('.') != std::string::npos) {
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  return text.empty() ? "0" : text;
}

int ClampInt(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

double ClampDouble(double value, double low, double high) {
  return value < low ? low : (value > high ? high : value);
}

bool ParseBool(std::string_view value, bool fallback) {
  if (value == "true" || value == "1" || value == "on") return true;
  if (value == "false" || value == "0" || value == "off") return false;
  return fallback;
}

int ParseInt(std::string_view value, int fallback) {
  try {
    return std::stoi(std::string(value));
  } catch (...) {
    return fallback;
  }
}

double ParseDouble(std::string_view value, double fallback) {
  try {
    return std::stod(std::string(value));
  } catch (...) {
    return fallback;
  }
}

const char *BackendName(pl::cosmetics::GlBackend backend) {
  switch (backend) {
    case pl::cosmetics::GlBackend::Gles:
      return "gles";
    case pl::cosmetics::GlBackend::Vulkan:
      return "vulkan";
    default:
      return "unknown";
  }
}

} // namespace

MasCeffectsMod &MasCeffectsMod::instance() {
  static MasCeffectsMod mod;
  return mod;
}

MasCeffectsMod::MasCeffectsMod() : self_(*ll::mod::NativeMod::current()) {}

bool MasCeffectsMod::load() {
  std::lock_guard<std::mutex> lock(configMutex_);
  config_.emplace();
  if (!config_->load()) {
    self_.getLogger().error("MasCeffects: failed to load config");
    config_.reset();
    return false;
  }
  publishConfigLocked();
  self_.getLogger().info("MasCeffects: loaded (version {})",
                         config_->value().version);
  return true;
}

bool MasCeffectsMod::enable() {
  if (!registerModMenu()) {
    self_.getLogger().error("MasCeffects: Mod Menu registration failed");
    return false;
  }

  // Registering the same callback twice is allowed by the SDK, so a repeated
  // enable would otherwise stack duplicate callbacks. The guard keeps enable
  // idempotent.
  if (!renderCallbackRegistered_.exchange(true, std::memory_order_acq_rel)) {
    if (!pl::render::RegisterRenderCallback(
            [this](const pl::render::RenderContext &context) {
              onRender(context);
            })) {
      renderCallbackRegistered_.store(false, std::memory_order_release);
      self_.getLogger().error(
          "MasCeffects: render callback registration failed");
      return false;
    }
  }

  enabled_.store(true, std::memory_order_release);

  // The entity feed is what makes the hitboxes and the death effect possible.
  // It is installed separately from the render callback so that a build where it
  // cannot resolve loses only those two features; the shockwave, trail and
  // status HUD keep working.
  if (!feed_.install()) {
    self_.getLogger().warn(
        "MasCeffects: entity feed unavailable ({}); hitboxes and the death "
        "effect are off, everything else still works",
        feed_.status());
  }

  self_.getLogger().info("MasCeffects: enabled");
  return true;
}


bool MasCeffectsMod::disable() {
  enabled_.store(false, std::memory_order_release);
  // The callback is deliberately left registered. The SDK's
  // UnregisterRenderCallbacks() only removes the *most recently* registered
  // callback (or every callback when passed true), so calling it here could
  // remove another mod's callback instead of this one. Gating on `enabled_`
  // stops all drawing, which is what disable() means; the callback itself is
  // released with the process.
  pl::modmenu::unregisterButton(kClearEffectsButtonId);
  pl::modmenu::unregisterButton(kShockwaveButtonId);
  pl::modmenu::unregisterModule(kModuleId);
  modMenuRegistered_.store(false, std::memory_order_release);

  self_.getLogger().info("MasCeffects: disabled");
  return true;
}

bool MasCeffectsMod::unload() {
  renderer_.shutdown();
  engine_.reset();
  {
    std::lock_guard<std::mutex> lock(configMutex_);
    config_.reset();
  }
  self_.getLogger().info("MasCeffects: unloaded");
  return true;
}

ConfigSnapshot MasCeffectsMod::renderConfig() {
  ConfigSnapshot snapshot;
  // A copy is only published after the first load, so an unpublished channel
  // means "not ready yet" and the defaults are the safe answer.
  if (configChannel_.read(configBytes_) &&
      configBytes_.size() == sizeof(ConfigSnapshot)) {
    std::memcpy(&snapshot, configBytes_.data(), sizeof(ConfigSnapshot));
  }
  return snapshot;
}

void MasCeffectsMod::publishConfigLocked() {
  if (!config_) return;
  const ConfigSnapshot snapshot = ConfigSnapshot::From(config_->value());
  const auto *raw = reinterpret_cast<const std::uint8_t *>(&snapshot);
  configChannel_.publish({raw, sizeof(ConfigSnapshot)});
}

bool MasCeffectsMod::saveConfigLocked(std::string_view reason) {
  if (!config_) return false;
  const bool saved = config_->save();
  if (saved) {
    self_.getLogger().info("MasCeffects: config saved after {}", reason);
  } else {
    self_.getLogger().warn("MasCeffects: config save failed after {}", reason);
  }
  return saved;
}

bool MasCeffectsMod::registerModMenu() {
  // The menu's defaults come from the persisted config, so it is read under the
  // lock here; the render thread uses the published snapshot instead.
  ConfigSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(configMutex_);
    snapshot = config_ ? ConfigSnapshot::From(config_->value())
                       : ConfigSnapshot{};
  }

  const bool module =
      pl::modmenu::ModuleBuilder(kModuleId, kModuleName)
          .modId(self_.getId())
          .description(
              "Visual PvP effects: mace smash shockwave, ender pearl trail, "
              "impact burst and a local hitbox.")
          .defaultEnabled(true)
          .onToggle(onToggleCallback)
          .config("shockwaveEnabled", "Mace Smash Shockwave",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.shockwaveEnabled ? "true" : "false")
          .config("shockwaveSegments", "Shockwave Segments",
                  pl::modmenu::ConfigType::SliderInt,
                  std::to_string(snapshot.shockwaveSegments),
                  std::to_string(kMinSegments), std::to_string(kMaxSegments))
          .config("shockwaveRadius", "Shockwave Radius",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.shockwaveRadius),
                  ToMenuValue(kMinRadius), ToMenuValue(kMaxRadius))
          .config("shockwaveLifeMs", "Shockwave Lifetime (ms)",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.shockwaveLifeMs),
                  ToMenuValue(kMinLifeMs), ToMenuValue(kMaxLifeMs))
          .config("shockwaveThickness", "Shockwave Thickness",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.shockwaveThickness),
                  ToMenuValue(kMinThickness), ToMenuValue(kMaxThickness))
          .config("shockwaveColor", "Shockwave Color",
                  pl::modmenu::ConfigType::Color, "#7FD4FF")
          .config("shockwaveAutoOnLanding", "Auto On Landing",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.shockwaveAutoOnLanding ? "true" : "false")
          .config("shockwaveMinFallSpeed", "Min Fall Speed (blocks/s)",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.shockwaveMinFallSpeed),
                  ToMenuValue(kMinFallSpeed), ToMenuValue(kMaxFallSpeed))
          .config("pearlTrailEnabled", "Ender Pearl Trail",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.pearlTrailEnabled ? "true" : "false")
          .config("pearlTrailColor", "Pearl Trail Color",
                  pl::modmenu::ConfigType::Color, "#C08CFF")
          .config("pearlTrailLifeMs", "Pearl Trail Lifetime (ms)",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.pearlTrailLifeMs),
                  ToMenuValue(kMinLifeMs), ToMenuValue(kMaxLifeMs))
          .config("pearlTrailWidth", "Pearl Trail Width",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.pearlTrailWidth),
                  ToMenuValue(kMinThickness), ToMenuValue(kMaxThickness))
          .config("pearlTrailMinJump", "Teleport Threshold (blocks)",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.pearlTrailMinJump), ToMenuValue(kMinJump),
                  ToMenuValue(kMaxJump))
          .config("impactBurstEnabled", "Impact Burst",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.impactBurstEnabled ? "true" : "false")
          .config("impactBurstColor", "Impact Burst Color",
                  pl::modmenu::ConfigType::Color, "#FF4D4D")
          .config("impactBurstLifeMs", "Impact Burst Lifetime (ms)",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.impactBurstLifeMs),
                  ToMenuValue(kMinLifeMs), ToMenuValue(kMaxLifeMs))
          .config("impactBurstParticles", "Impact Burst Particles",
                  pl::modmenu::ConfigType::SliderInt,
                  std::to_string(snapshot.impactBurstParticles),
                  std::to_string(kMinParticles), std::to_string(kMaxParticles))
          .config("impactBurstRadius", "Impact Burst Radius",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.impactBurstRadius),
                  ToMenuValue(kMinRadius), ToMenuValue(kMaxRadius))
          .config("deathEffectEnabled", "Player Death Effect",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.deathEffectEnabled ? "true" : "false")
          .config("deathEffectColor", "Death Effect Color",
                  pl::modmenu::ConfigType::Color, "#FF4D4D")
          .config("deathEffectLifeMs", "Death Effect Lifetime (ms)",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.deathEffectLifeMs),
                  ToMenuValue(kMinLifeMs), ToMenuValue(kMaxLifeMs))
          .config("deathEffectParticles", "Death Effect Particles",
                  pl::modmenu::ConfigType::SliderInt,
                  std::to_string(snapshot.deathEffectParticles),
                  std::to_string(kMinParticles), std::to_string(kMaxParticles))
          .config("deathEffectRadius", "Death Effect Radius",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.deathEffectRadius),
                  ToMenuValue(kMinRadius), ToMenuValue(kMaxRadius))
          .config("deathEffectMarkerSize", "Death Marker Size",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.deathEffectMarkerSize),
                  ToMenuValue(kMinBox), ToMenuValue(kMaxBox))
          .config("deathEffectRise", "Death Marker Rise",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.deathEffectRise), ToMenuValue(kMinRise),
                  ToMenuValue(kMaxRise))
          .config("deathEffectMaxConcurrent", "Max Death Effects",
                  pl::modmenu::ConfigType::SliderInt,
                  std::to_string(snapshot.deathEffectMaxConcurrent),
                  std::to_string(kMinConcurrent), std::to_string(kMaxConcurrent))
          .config("hitboxEnabled", "Custom Hitboxes",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.hitboxEnabled ? "true" : "false")
          .config("hitboxColor", "Player Hitbox Color",
                  pl::modmenu::ConfigType::Color, "#FFFFFF")
          .config("hitboxOpacity", "Player Hitbox Opacity",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.hitboxOpacity),
                  ToMenuValue(kMinOpacity), ToMenuValue(kMaxOpacity))
          .config("hitboxMobOpacity", "Mob Hitbox Opacity",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.hitboxMobOpacity),
                  ToMenuValue(kMinOpacity), ToMenuValue(kMaxOpacity))
          .config("hitboxSelfOpacity", "Self Hitbox Opacity",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.hitboxSelfOpacity),
                  ToMenuValue(kMinOpacity), ToMenuValue(kMaxOpacity))
          .config("hitboxShowLookLine", "Hitbox Look Line",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.hitboxShowLookLine ? "true" : "false")
          .config("hitboxFadeDistance", "Hitbox Fade Distance",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.hitboxFadeDistance),
                  ToMenuValue(kMinFadeDistance), ToMenuValue(kMaxFadeDistance))
          .config("hitboxFadeRange", "Hitbox Fade Range",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.hitboxFadeRange),
                  ToMenuValue(kMinFadeRange), ToMenuValue(kMaxFadeRange))
          .config("hitboxMaxEntities", "Max Hitboxes",
                  pl::modmenu::ConfigType::SliderInt,
                  std::to_string(snapshot.hitboxMaxEntities),
                  std::to_string(kMinMaxEntities),
                  std::to_string(kMaxMaxEntities))
          .config("showStatusHud", "Status HUD",
                  pl::modmenu::ConfigType::Toggle,
                  snapshot.showStatusHud ? "true" : "false")
          .config("statusHudScale", "Status HUD Scale",
                  pl::modmenu::ConfigType::SliderFloat,
                  ToMenuValue(snapshot.statusHudScale), ToMenuValue(kMinScale),
                  ToMenuValue(kMaxScale))
          .config("showDiagnostics", "Diagnostics",
                  pl::modmenu::ConfigType::Toggle, "false")
          .onConfigChanged(onConfigCallback)
          .registerModule();

  if (!module) return false;

  const bool clearButton =
      pl::modmenu::ButtonBuilder(kClearEffectsButtonId, "Clear Effects")
          .moduleId(kModuleId)
          .modId(self_.getId())
          .label("Clear")
          .behavior(pl::modmenu::ButtonBehavior::Click)
          .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
          .styleColors(0xCC24282CU, 0xFF7FD4FFU, 0x997FD4FFU)
          .textColor(0xFFFFFFFFU)
          .activeTextColor(0xFF000000U)
          .onEvent(onButtonCallback)
          .registerButton();

  const bool smashButton =
      pl::modmenu::ButtonBuilder(kShockwaveButtonId, "Test Shockwave")
          .moduleId(kModuleId)
          .modId(self_.getId())
          .label("Smash")
          .behavior(pl::modmenu::ButtonBehavior::Click)
          .sizeScale(1.6F, 1.0F)
          .onEvent(onButtonCallback)
          .registerButton();

  modMenuRegistered_.store(clearButton && smashButton,
                           std::memory_order_release);
  return clearButton && smashButton;
}

void MasCeffectsMod::onToggleCallback(std::string_view moduleId, bool enabled) {
  if (moduleId != kModuleId) return;
  instance().self_.getLogger().info("MasCeffects: module {}",
                                    enabled ? "enabled" : "disabled");
}

void MasCeffectsMod::onConfigCallback(std::string_view moduleId,
                                      std::string_view key,
                                      std::string_view value) {
  if (moduleId != kModuleId || key.empty()) return;
  instance().applyConfigChange(key, value);
}

void MasCeffectsMod::onButtonCallback(std::string_view buttonId,
                                      pl::modmenu::ButtonEvent event,
                                      float value) {
  (void)value;
  if (event != pl::modmenu::ButtonEvent::Click) return;
  MasCeffectsMod &mod = instance();
  if (buttonId == kClearEffectsButtonId) {
    mod.clearRequested_.store(true, std::memory_order_release);
  } else if (buttonId == kShockwaveButtonId) {
    mod.manualSmashRequested_.store(true, std::memory_order_release);
  }
}

void MasCeffectsMod::applyConfigChange(std::string_view key,
                                       std::string_view value) {
  std::lock_guard<std::mutex> lock(configMutex_);
  if (!config_) return;
  MasEffectConfig &config = config_->value();

  if (key == "shockwaveEnabled") {
    config.shockwaveEnabled = ParseBool(value, config.shockwaveEnabled);
  } else if (key == "shockwaveSegments") {
    config.shockwaveSegments =
        ClampInt(ParseInt(value, config.shockwaveSegments), kMinSegments,
                 kMaxSegments);
  } else if (key == "shockwaveRadius") {
    config.shockwaveRadius = ClampDouble(ParseDouble(value, config.shockwaveRadius),
                                         kMinRadius, kMaxRadius);
  } else if (key == "shockwaveLifeMs") {
    config.shockwaveLifeMs =
        ClampDouble(ParseDouble(value, config.shockwaveLifeMs), kMinLifeMs,
                    kMaxLifeMs);
  } else if (key == "shockwaveThickness") {
    config.shockwaveThickness =
        ClampDouble(ParseDouble(value, config.shockwaveThickness), kMinThickness,
                    kMaxThickness);
  } else if (key == "shockwaveColor") {
    config.shockwaveColor = std::string(value);
  } else if (key == "shockwaveAutoOnLanding") {
    config.shockwaveAutoOnLanding =
        ParseBool(value, config.shockwaveAutoOnLanding);
  } else if (key == "shockwaveMinFallSpeed") {
    config.shockwaveMinFallSpeed =
        ClampDouble(ParseDouble(value, config.shockwaveMinFallSpeed),
                    kMinFallSpeed, kMaxFallSpeed);
  } else if (key == "pearlTrailEnabled") {
    config.pearlTrailEnabled = ParseBool(value, config.pearlTrailEnabled);
  } else if (key == "pearlTrailColor") {
    config.pearlTrailColor = std::string(value);
  } else if (key == "pearlTrailLifeMs") {
    config.pearlTrailLifeMs =
        ClampDouble(ParseDouble(value, config.pearlTrailLifeMs), kMinLifeMs,
                    kMaxLifeMs);
  } else if (key == "pearlTrailWidth") {
    config.pearlTrailWidth =
        ClampDouble(ParseDouble(value, config.pearlTrailWidth), kMinThickness,
                    kMaxThickness);
  } else if (key == "pearlTrailMinJump") {
    config.pearlTrailMinJump =
        ClampDouble(ParseDouble(value, config.pearlTrailMinJump), kMinJump,
                    kMaxJump);
  } else if (key == "impactBurstEnabled") {
    config.impactBurstEnabled = ParseBool(value, config.impactBurstEnabled);
  } else if (key == "impactBurstColor") {
    config.impactBurstColor = std::string(value);
  } else if (key == "impactBurstLifeMs") {
    config.impactBurstLifeMs =
        ClampDouble(ParseDouble(value, config.impactBurstLifeMs), kMinLifeMs,
                    kMaxLifeMs);
  } else if (key == "impactBurstParticles") {
    config.impactBurstParticles =
        ClampInt(ParseInt(value, config.impactBurstParticles), kMinParticles,
                 kMaxParticles);
  } else if (key == "impactBurstRadius") {
    config.impactBurstRadius =
        ClampDouble(ParseDouble(value, config.impactBurstRadius), kMinRadius,
                    kMaxRadius);
  } else if (key == "deathEffectEnabled") {
    config.deathEffectEnabled = ParseBool(value, config.deathEffectEnabled);
  } else if (key == "deathEffectColor") {
    config.deathEffectColor = std::string(value);
  } else if (key == "deathEffectLifeMs") {
    config.deathEffectLifeMs =
        ClampDouble(ParseDouble(value, config.deathEffectLifeMs), kMinLifeMs,
                    kMaxLifeMs);
  } else if (key == "deathEffectParticles") {
    config.deathEffectParticles =
        ClampInt(ParseInt(value, config.deathEffectParticles), kMinParticles,
                 kMaxParticles);
  } else if (key == "deathEffectRadius") {
    config.deathEffectRadius =
        ClampDouble(ParseDouble(value, config.deathEffectRadius), kMinRadius,
                    kMaxRadius);
  } else if (key == "deathEffectMarkerSize") {
    config.deathEffectMarkerSize =
        ClampDouble(ParseDouble(value, config.deathEffectMarkerSize), kMinBox,
                    kMaxBox);
  } else if (key == "deathEffectRise") {
    config.deathEffectRise = ClampDouble(ParseDouble(value, config.deathEffectRise),
                                         kMinRise, kMaxRise);
  } else if (key == "deathEffectMaxConcurrent") {
    config.deathEffectMaxConcurrent =
        ClampInt(ParseInt(value, config.deathEffectMaxConcurrent), kMinConcurrent,
                 kMaxConcurrent);
  } else if (key == "hitboxEnabled") {
    config.hitboxEnabled = ParseBool(value, config.hitboxEnabled);
  } else if (key == "hitboxColor") {
    config.hitboxColor = std::string(value);
  } else if (key == "hitboxOpacity") {
    config.hitboxOpacity = ClampDouble(ParseDouble(value, config.hitboxOpacity),
                                       kMinOpacity, kMaxOpacity);
  } else if (key == "hitboxMobOpacity") {
    config.hitboxMobOpacity =
        ClampDouble(ParseDouble(value, config.hitboxMobOpacity), kMinOpacity,
                    kMaxOpacity);
  } else if (key == "hitboxSelfOpacity") {
    config.hitboxSelfOpacity =
        ClampDouble(ParseDouble(value, config.hitboxSelfOpacity), kMinOpacity,
                    kMaxOpacity);
  } else if (key == "hitboxShowLookLine") {
    config.hitboxShowLookLine = ParseBool(value, config.hitboxShowLookLine);
  } else if (key == "hitboxFadeDistance") {
    config.hitboxFadeDistance =
        ClampDouble(ParseDouble(value, config.hitboxFadeDistance),
                    kMinFadeDistance, kMaxFadeDistance);
  } else if (key == "hitboxFadeRange") {
    config.hitboxFadeRange =
        ClampDouble(ParseDouble(value, config.hitboxFadeRange), kMinFadeRange,
                    kMaxFadeRange);
  } else if (key == "hitboxMaxEntities") {
    config.hitboxMaxEntities =
        ClampInt(ParseInt(value, config.hitboxMaxEntities), kMinMaxEntities,
                 kMaxMaxEntities);
  } else if (key == "showStatusHud") {
    config.showStatusHud = ParseBool(value, config.showStatusHud);
  } else if (key == "statusHudScale") {
    config.statusHudScale = ClampDouble(ParseDouble(value, config.statusHudScale),
                                        kMinScale, kMaxScale);
  } else if (key == "showDiagnostics") {
    config.showDiagnostics = ParseBool(value, config.showDiagnostics);
  } else {
    return;
  }

  // Every accepted edit is published for the render thread and persisted, so a
  // Mod Menu change takes effect immediately and survives a restart.
  publishConfigLocked();
  saveConfigLocked(key);
}

void MasCeffectsMod::onRender(const pl::render::RenderContext &context) {
  if (!enabled_.load(std::memory_order_acquire)) return;

  const std::int64_t nowMs = MonotonicMs();
  const ConfigSnapshot config = renderConfig();

  // A manual trigger from the Mod Menu button spawns a shockwave at the player,
  // which is the only way to preview the effect without jumping off something.
  if (manualSmashRequested_.exchange(false, std::memory_order_acq_rel)) {
    float position[3] = {0.0F, 0.0F, 0.0F};
    if (pl::render::ReadLocalPlayerPosition(position)) {
      engine_.requestShockwave(position[0], position[1], position[2], config,
                               nowMs);
    }
  }
  if (clearRequested_.exchange(false, std::memory_order_acq_rel)) {
    engine_.reset();
  }

  float position[3] = {0.0F, 0.0F, 0.0F};
  const bool havePlayer = pl::render::ReadLocalPlayerPosition(position);
  if (havePlayer) {
    engine_.update(context.frameTick, nowMs, position[0], position[1],
                   position[2], config);
  }

  // The entities the game handed us while it rendered this frame. Draining also
  // empties the feed's buffer, so capture stays bounded to one frame.
  std::vector<GameEntity> entities;
  if (feed_.installed()) {
    entities = feed_.drain();
    if (!entities.empty()) {
      engine_.observeEntities(entities, config, nowMs);
    }
  }

  // The camera matrix is optional; without it nothing can be placed in the
  // world, so the pass is skipped rather than drawn at a guessed transform.
  pl::cosmetics::Mat4 viewProjection = context.viewProjection;
  if (!context.hasViewProjection) {
    if (context.viewportWidth <= 0 || context.viewportHeight <= 0) {
      submitHud();
      return;
    }
    const float aspect = static_cast<float>(context.viewportWidth) /
                         static_cast<float>(context.viewportHeight);
    if (!pl::render::ComputeViewProjection(70.0F, aspect, 0.05F, 512.0F,
                                           viewProjection)) {
      submitHud();
      return;
    }
  }

  if (renderer_.EnsureReady()) {
    const std::vector<DrawBatch> batches = engine_.collect(nowMs, config);
    for (const DrawBatch &batch : batches) {
      renderer_.draw(viewProjection, batch.segments, batch.color);
    }

    if (config.hitboxEnabled && havePlayer) {
      drawEntityHitboxes(entities, position, viewProjection, config);
    }
  }

  submitHud();
}

void MasCeffectsMod::drawEntityHitboxes(
    const std::vector<GameEntity> &entities, const float playerPosition[3],
    const pl::cosmetics::Mat4 &viewProjection, const ConfigSnapshot &config) {
  const int limit = std::max(config.hitboxMaxEntities, 1);
  int drawn = 0;

  for (const GameEntity &entity : entities) {
    if (drawn >= limit) break;
    if (!entity.boundsValid) continue;

    const double dx = static_cast<double>(entity.x) - playerPosition[0];
    const double dy = static_cast<double>(entity.y) - playerPosition[1];
    const double dz = static_cast<double>(entity.z) - playerPosition[2];
    const auto distance =
        static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));

    // Matches the Java mod's fade: solid up close, ramping out past the fade
    // distance. The player's own box uses its own opacity so it stays subtle.
    const float fade = HitboxAlpha(distance, config.hitboxFadeDistance,
                                   config.hitboxFadeRange);
    if (fade <= 0.0F) continue;

    Rgba color = config.hitboxColor;
    double opacity = config.hitboxOpacity;
    if (!entity.isPlayer) {
      opacity = config.hitboxMobOpacity;
    } else if (entity.isLocalPlayer) {
      opacity = config.hitboxSelfOpacity;
    }

    const float alpha = static_cast<float>(opacity) * fade;
    const std::vector<Segment> box =
        BuildBoxWireframe(entity.bounds[0], entity.bounds[1], entity.bounds[2],
                          entity.bounds[3], entity.bounds[4], entity.bounds[5]);
    renderer_.draw(viewProjection, box, ScaleAlpha(color, alpha));
    ++drawn;

    if (!config.hitboxShowLookLine || !entity.rotationValid) continue;
    const float top = (entity.bounds[4] + entity.bounds[1]) * 0.5F;
    const std::vector<Segment> look = {
        BuildLookLine(entity.x, top, entity.z, entity.yaw, entity.pitch, 1.5)};
    renderer_.draw(viewProjection, look, ScaleAlpha(color, alpha));
  }
}

void MasCeffectsMod::submitHud() {
  const ConfigSnapshot config = renderConfig();
  if (!config.showStatusHud) {
    if (hasHud_) {
      pl::modmenu::submitDrawCommands(kModuleId, {});
      hasHud_ = false;
    }
    return;
  }

  // Throttled: the readout is text, so 10 Hz is indistinguishable from per-frame
  // while costing a fraction of the render-thread time and menu-lock traffic.
  const std::int64_t nowMs = MonotonicMs();
  if (hasHud_ && nowMs - lastHudMs_ < kHudIntervalMs) return;
  lastHudMs_ = nowMs;
  hasHud_ = true;

  // A font is required for text commands; without one the overlay falls back to
  // a bar, so the failure is logged rather than silently swallowed.
  hud_.EnsureFont();

  HudStats stats;
  stats.renderLive = pl::render::IsRenderCallbackLive();
  stats.pipelineReady = renderer_.ready();
  stats.frameTick = pl::render::RenderFrameTick();
  stats.smashCount = engine_.smashCount();
  stats.teleportCount = engine_.teleportCount();
  stats.deathCount = engine_.deathCount();
  stats.liveEffects = engine_.liveCount();
  stats.backend = BackendName(pl::cosmetics::GlesBackend());
  stats.lastGlError = pl::cosmetics::LastGlesError();
  stats.entityFeedLive = feed_.live();
  stats.entityFeedStatus = feed_.status();
  stats.capturedEntities = feed_.captureCount();
  stats.showDiagnostics = config.showDiagnostics;

  pl::modmenu::submitDrawCommands(kModuleId,
                                  hud_.build(stats, config.statusHudScale));
}

} // namespace masc

PL_REGISTER_MOD(masc::MasCeffectsMod, masc::MasCeffectsMod::instance())
