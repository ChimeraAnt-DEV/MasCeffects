#pragma once

/**
 * @file GameFeed.hpp
 * @brief Reads entities and player health out of the running game.
 *
 * Two things the effects need are not in the preloader's public SDK surface, so
 * this reads them from the game directly through a single hook on the entity
 * render dispatcher:
 *
 * <ol>
 *   <li><b>Entities, for the hitboxes.</b> The Java mod's hitbox mixin hooks
 *       {@code EntityRenderDispatcher::renderHitbox}, which the game calls once
 *       per entity as it renders, handing the entity over as an argument. That
 *       is why no entity list is needed: the game already iterates them and
 *       tells us. The same hook point exists in Bedrock's
 *       {@code ActorRenderDispatcher}, and the detour captures each entity
 *       pointer as it goes past.</li>
 *   <li><b>Player health, for the death effect.</b> The Java mod watches
 *       {@code player.deathTime}. Bedrock exposes no death event, so health is
 *       read off each tracked entity and a drop to zero is a death.</li>
 * </ol>
 *
 * <p><b>This is the part that has to be right or not run at all.</b> The
 * offsets below are the ones the preloader already documents and uses for its
 * own local-player feed; they are verified for the 1.26.x builds. Everything is
 * gated:
 *
 * <ul>
 *   <li>The hook only installs when the render dispatcher's vtable slot resolves
 *       <em>and</em> lands on an executable address.</li>
 *   <li>A captured entity is only published when its type name looks like a real
 *       RTTI name, so a wrong slot produces no entities instead of garbage.</li>
 *   <li>A health reading outside {@code (0, maxHealth]} is treated as garbage and
 *       ignored, so a wrong offset cannot look like a live player.</li>
 * </ul>
 *
 * <p>If any of that fails, {@link live} stays false, no entities are published,
 * no death fires, and the rest of the mod keeps working. The diagnostics line in
 * the status HUD reports which state it is in.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace masc {

/** @brief The game module the feed resolves against. */
inline constexpr const char *kGameModule = "libminecraftpe.so";

/** @brief One entity captured from the game's render pass. */
struct GameEntity {
  /// The entity's address, used as a stable identity across frames.
  std::uintptr_t entityId = 0;
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  /// A 6-float AABB: minX, minY, minZ, maxX, maxY, maxZ. Filled when the box
  /// could be read; otherwise `boundsValid` stays false and no box is drawn.
  float bounds[6] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
  bool boundsValid = false;
  /// True when the entity's type name is a player type.
  bool isPlayer = false;
  /// True when the entity is the local player.
  bool isLocalPlayer = false;
  /// Current health, or a negative value when it could not be read.
  float health = -1.0F;
  /// The entity's view rotation in degrees, when readable.
  float yaw = 0.0F;
  float pitch = 0.0F;
  bool rotationValid = false;
};

/**
 * @brief The entity/health feed.
 *
 * All methods are called from the render thread. {@link capture} runs inside the
 * detour and does no allocation.
 */
class GameFeed {
public:
  GameFeed() = default;

  /**
   * @brief Resolves the player-render entry and installs the capture detour.
   *
   * Safe to call repeatedly; the work happens once. Failure is sticky, so a
   * build whose slot does not resolve does not retry the scan every frame.
   *
   * <p>The entry is resolved by the diagnostic string its body references rather
   * than by a slot index, because the index moves between builds while the
   * string does not. If that scan finds nothing, the verified index the client's
   * rules ship is used as a fallback.
   */
  bool install();

  /** @brief True once the detour is installed. */
  [[nodiscard]] bool installed() const { return installed_; }

  /** @brief True once at least one entity has been captured this session. */
  [[nodiscard]] bool live() const { return live_; }

  /** @brief A human-readable reason when the feed is unavailable. */
  [[nodiscard]] const std::string &status() const { return status_; }

  /**
   * @brief Copies the entities captured since the last call and resets the frame
   *        list. Called once per frame from the render callback.
   */
  std::vector<GameEntity> drain();

  /** @brief Enables or disables entity capture. */
  void setCaptureEnabled(bool enabled) { captureEnabled_ = enabled; }

  /** @brief True while the detour should publish what it sees. */
  [[nodiscard]] bool capturing() const { return captureEnabled_; }

  /** @brief The number of entities captured this session, for diagnostics. */
  [[nodiscard]] std::uint64_t captureCount() const { return captureCount_; }

  /** @brief The number of health readings rejected as non-finite. */
  [[nodiscard]] std::uint64_t implausibleHealthCount() const {
    return implausibleHealthCount_;
  }

  // ---- Called by the detour; not for general use. ------------------------
  void onEntityCaptured(std::uintptr_t entity, std::string_view typeName);

private:
  bool installed_ = false;
  bool attempted_ = false;
  bool live_ = false;
  bool captureEnabled_ = true;
  std::string status_ = "not installed";

  // Written by the detour on the render thread, drained by the render callback
  // on the same thread, so no synchronisation is needed.
  std::vector<GameEntity> frame_;
  std::uintptr_t localPlayer_ = 0;
  std::uint64_t captureCount_ = 0;
  std::uint64_t implausibleHealthCount_ = 0;
};

/** @brief The process-wide feed the detour publishes into. */
GameFeed &SharedGameFeed();

} // namespace masc
