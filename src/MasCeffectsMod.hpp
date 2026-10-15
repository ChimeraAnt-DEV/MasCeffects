#pragma once

/**
 * @file MasCeffectsMod.hpp
 * @brief The mod lifecycle object.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <pl/Config.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Render.hpp>
#include <pl/ipc/SnapshotChannel.hpp>

#include "EffectEngine.hpp"
#include "GameFeed.hpp"
#include "HudOverlay.hpp"
#include "MasEffectConfig.hpp"
#include "WorldRenderer.hpp"

namespace masc {

/**
 * @brief MasCeffects: visual PvP effects for Minecraft Bedrock.
 *
 * One long-lived instance is registered with `PL_REGISTER_MOD`. It owns the
 * typed config, the Mod Menu surface, the effect simulation and the GL pipeline.
 *
 * <p><b>Threading.</b> The render callback runs on the render thread while
 * Mod Menu callbacks and lifecycle calls arrive on other threads, so the config
 * is guarded by a mutex and the render path only ever reads a copy taken under
 * it. Nothing in the render path allocates unboundedly or blocks.
 *
 * <p><b>Fail-closed.</b> If the render seam never comes live, or the GL pipeline
 * cannot be built, the mod draws nothing and the game runs exactly as it did
 * without it. The status HUD says which of those happened instead of leaving the
 * user to guess.
 */
class MasCeffectsMod {
public:
  static MasCeffectsMod &instance();

  explicit MasCeffectsMod();

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return self_; }

  bool load();
  bool enable();
  bool disable();
  bool unload();

private:
  static constexpr const char *kModuleId = "masceffects.effects";
  static constexpr const char *kModuleName = "Mas Effects";
  static constexpr const char *kClearEffectsButtonId =
      "masceffects.clear_button";
  static constexpr const char *kShockwaveButtonId = "masceffects.smash_button";

  /// Reads the resolved config on the render thread, without locking. The
  /// channel is the SDK's own lock-free hand-off, so the render callback never
  /// waits on a Mod Menu edit.
  ConfigSnapshot renderConfig();

  /// Reads the resolved config and publishes it for the render thread. Call
  /// with `configMutex_` held.
  void publishConfigLocked();

  /// Persists the config. The caller must already hold `configMutex_`.
  bool saveConfigLocked(std::string_view reason);

  /// Registers the Mod Menu module and its buttons. Called from `enable()`.
  bool registerModMenu();

  /// The per-frame render callback; runs on the render thread.
  void onRender(const pl::render::RenderContext &context);

  /// Draws a box for every entity the game handed us this frame.
  void drawEntityHitboxes(const std::vector<GameEntity> &entities,
                          const float playerPosition[3],
                          const pl::cosmetics::Mat4 &viewProjection,
                          const ConfigSnapshot &config);

  /// Builds and submits the status readout.
  void submitHud();

  void applyConfigChange(std::string_view key, std::string_view value);

  static void onToggleCallback(std::string_view moduleId, bool enabled);
  static void onConfigCallback(std::string_view moduleId, std::string_view key,
                               std::string_view value);
  static void onButtonCallback(std::string_view buttonId,
                               pl::modmenu::ButtonEvent event, float value);

  ll::mod::NativeMod &self_;

  mutable std::mutex configMutex_;
  std::optional<pl::config::ConfigFile<MasEffectConfig>> config_;

  /// Reused by {@link renderConfig} so the render thread never allocates while
  /// reading the published config.
  std::vector<std::uint8_t> configBytes_;

  /// The resolved config, published for the render thread to read without
  /// taking `configMutex_`. A Mod Menu edit republishes it; the render thread
  /// only ever reads a consistent copy.
  pl::ipc::SnapshotChannel configChannel_{sizeof(ConfigSnapshot)};

  // Render-thread-owned state. Never touched from a callback thread.
  EffectEngine engine_;
  WorldRenderer renderer_;
  HudOverlay hud_;
  GameFeed feed_;

  std::atomic_bool renderCallbackRegistered_{false};
  std::atomic_bool modMenuRegistered_{false};
  std::atomic_bool enabled_{false};
  std::atomic_bool clearRequested_{false};
  std::atomic_bool manualSmashRequested_{false};

  /// The HUD is throttled to this interval. It is a text readout, so redrawing
  /// it every frame would cost render-thread time and menu-lock contention for
  /// no visible benefit.
  static constexpr std::int64_t kHudIntervalMs = 100;
  std::int64_t lastHudMs_ = 0;
  bool hasHud_ = false;
};

} // namespace masc
