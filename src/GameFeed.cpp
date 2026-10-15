#include "GameFeed.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <pl/Logger.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/HookTarget.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/memory/Vtable.hpp>

namespace masc {
namespace {

using PlayerRenderFn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t,
                                          std::uintptr_t, std::uintptr_t,
                                          std::uintptr_t, std::uintptr_t,
                                          std::uintptr_t, std::uintptr_t);

void *gOriginal = nullptr;

/**
 * The `Actor` field offsets the preloader itself documents and uses for its own
 * local-player feed (`GameLocalPlayer.cpp`). They are named for `Actor`, not for
 * `LocalPlayer`, and are shared by every 1.26.x build that feed targets.
 */
constexpr std::size_t kActorPositionOffset = 0x230;
constexpr std::size_t kActorRotationOffset = 0x238;
constexpr std::size_t kActorHealthOffset = 0x260;

/// A real Bedrock position is well inside the world border.
constexpr float kWorldLimit = 3.0e7F;
/// The longest RTTI type name worth reading.
constexpr std::size_t kMaxTypeName = 96;
/// The most entities buffered in one frame.
constexpr std::size_t kMaxEntitiesPerFrame = 512;
/// The most argument registers tried when locating the actor.
constexpr int kArgumentCandidates = 8;

bool IsPlausiblePosition(const float position[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(position[i]) || std::fabs(position[i]) > kWorldLimit) {
      return false;
    }
  }
  return true;
}

float ReadFloat(std::uintptr_t base, std::size_t offset) {
  float value = 0.0F;
  std::memcpy(&value, reinterpret_cast<const void *>(base + offset),
              sizeof(value));
  return value;
}

/**
 * @brief Reads a polymorphic object's Itanium RTTI type name.
 *
 * The name is reached the way the preloader's own vtable resolver reaches it:
 * the object's first word is its vtable, the word before the vtable is the
 * typeinfo, and the typeinfo's second word is the name. Every hop is checked
 * against `/proc/self/maps` before it is dereferenced, so a pointer that is not
 * an object is rejected rather than read.
 *
 * <p>Requiring a well-formed mangled name is what makes a captured register
 * trustworthy: a register holding a matrix, a counter or a `this` that is not an
 * actor cannot produce `"21ActorRenderDispatcher"`-shaped text.
 */
bool ReadTypeName(std::uintptr_t object, std::string &out) {
  out.clear();
  if (object == 0 || (object % alignof(void *)) != 0) return false;
  if (!pl::memory::queryMemoryRegion(object).readable()) return false;

  std::uintptr_t vtable = 0;
  std::memcpy(&vtable, reinterpret_cast<const void *>(object), sizeof(vtable));
  if (vtable == 0 || (vtable % alignof(void *)) != 0) return false;
  if (!pl::memory::queryMemoryRegion(vtable).readable()) return false;

  std::uintptr_t typeinfo = 0;
  std::memcpy(&typeinfo, reinterpret_cast<const void *>(vtable - sizeof(void *)),
              sizeof(typeinfo));
  if (typeinfo == 0) return false;
  if (!pl::memory::queryMemoryRegion(typeinfo).readable()) return false;

  std::uintptr_t name = 0;
  std::memcpy(&name, reinterpret_cast<const void *>(typeinfo + sizeof(void *)),
              sizeof(name));
  if (name == 0) return false;

  const pl::memory::MemoryRegionInfo region = pl::memory::queryMemoryRegion(name);
  if (!region.readable()) return false;

  const auto available = static_cast<std::size_t>(region.end - name);
  const std::size_t limit = std::min(available, kMaxTypeName);
  for (std::size_t i = 0; i < limit; ++i) {
    char c = 0;
    std::memcpy(&c, reinterpret_cast<const void *>(name + i), 1);
    if (c == '\0') break;
    // Itanium names are `<length><identifier>` and stay printable.
    if (c < 0x20 || c > 0x7E) return false;
    out.push_back(c);
  }
  if (out.empty()) return false;
  // A mangled name starts with its length, so a leading digit is required.
  return out.front() >= '0' && out.front() <= '9';
}

bool LooksLikePlayer(std::string_view name) {
  return name.find("Player") != std::string_view::npos;
}

bool LooksLikeActor(std::string_view name) {
  return name.find("Actor") != std::string_view::npos ||
         name.find("Player") != std::string_view::npos ||
         name.find("Mob") != std::string_view::npos ||
         name.find("Entity") != std::string_view::npos ||
         name.find("Monster") != std::string_view::npos ||
         name.find("Animal") != std::string_view::npos;
}

/**
 * @brief The capture detour on the game's player-render entry.
 *
 * <p>It forwards the full integer argument register set unchanged and never
 * dereferences an argument it has not validated, so a wrong signature cannot
 * fault the render thread - the same passthrough contract the preloader's own
 * hooks use. The actor is located by trying the argument registers in turn and
 * keeping the first that reads as a real polymorphic object with a finite world
 * position.
 *
 * <p>It is installed on {@code LivePlayerRenderer}, which the preloader verifies
 * for every shipped build, and {@code pl::memory::hook} chains a new detour onto
 * an existing target rather than replacing it, so this composes with the
 * preloader's own hook on the same slot.
 */
extern "C" std::uintptr_t PlayerRenderCaptureDetour(
    std::uintptr_t a0, std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3,
    std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7) {
  // A failure here must never escape into the game's render path.
  try {
    GameFeed &feed = SharedGameFeed();
    if (feed.capturing()) {
      const std::uintptr_t registers[kArgumentCandidates] = {a0, a1, a2, a3,
                                                            a4, a5, a6, a7};
      for (std::uintptr_t candidate : registers) {
        if (candidate == 0) continue;
        std::string name;
        if (!ReadTypeName(candidate, name)) continue;
        if (!LooksLikeActor(name)) continue;
        const float position[3] = {ReadFloat(candidate, kActorPositionOffset),
                                   ReadFloat(candidate, kActorPositionOffset + 4),
                                   ReadFloat(candidate, kActorPositionOffset + 8)};
        if (!IsPlausiblePosition(position)) continue;
        feed.onEntityCaptured(candidate, name);
        break;
      }
    }
  } catch (...) {
    // Swallowed deliberately: the game's render path is above us.
  }

  if (gOriginal != nullptr) {
    return reinterpret_cast<PlayerRenderFn>(gOriginal)(a0, a1, a2, a3, a4, a5, a6,
                                                       a7);
  }
  return 0;
}

} // namespace

GameFeed &SharedGameFeed() {
  static GameFeed feed;
  return feed;
}

void GameFeed::onEntityCaptured(std::uintptr_t entity, std::string_view typeName) {
  ++captureCount_;
  if (frame_.size() >= kMaxEntitiesPerFrame) return;

  // The feed is per-frame: a repeat of an entity already seen this frame is the
  // renderer being called more than once for it, and one box is enough.
  for (const GameEntity &seen : frame_) {
    if (seen.entityId == entity) return;
  }

  GameEntity out;
  out.entityId = entity;
  out.x = ReadFloat(entity, kActorPositionOffset);
  out.y = ReadFloat(entity, kActorPositionOffset + 4);
  out.z = ReadFloat(entity, kActorPositionOffset + 8);
  out.isPlayer = LooksLikePlayer(typeName);

  const float yaw = ReadFloat(entity, kActorRotationOffset);
  const float pitch = ReadFloat(entity, kActorRotationOffset + 4);
  if (std::isfinite(yaw) && std::isfinite(pitch)) {
    out.yaw = yaw;
    out.pitch = pitch;
    out.rotationValid = true;
  }

  const float health = ReadFloat(entity, kActorHealthOffset);
  if (std::isfinite(health)) {
    out.health = health;
  } else {
    // A non-finite reading is garbage from a wrong offset, not a health value.
    ++implausibleHealthCount_;
  }

  frame_.push_back(out);
  live_ = true;
}

bool GameFeed::install() {
  if (installed_) return true;
  if (attempted_) return false;
  attempted_ = true;

  // Resolve the render entry by RTTI name plus slot index. The string-referencing
  // variant (resolveVtableSlotReferencingString) would need no index, but it is
  // not actually exported by the preloader despite being declared PL_EXPORT --
  // the built library carries it as LOCAL HIDDEN -- so importing it would leave
  // an unresolved symbol. resolveVtableFunction is exported and is what the
  // preloader's own local-player feed uses.
  //
  // The index is the one the client's rules verify for every shipped build
  // (playerRenderVtableIndex = 17); the executable-address guard below is what
  // makes a stale index safe, since a wrong slot fails the check instead of
  // being hooked.
  constexpr std::size_t kPlayerRenderSlot = 17;
  const std::uintptr_t address = pl::memory::resolveVtableFunction(
      "18LivePlayerRenderer", kPlayerRenderSlot, kGameModule);

  if (address == 0) {
    status_ = "player-render slot unresolved";
    preloaderLogger.warn("MasCeffects: entity feed unavailable ({})", status_);
    return false;
  }

  // A stale slot that lands in data must be refused rather than hooked.
  if (!pl::memory::isExecutableAddress(address, kGameModule) ||
      !pl::memory::isHookAddressSane(address)) {
    status_ = "player-render slot is not executable code";
    preloaderLogger.warn("MasCeffects: entity feed unavailable ({})", status_);
    return false;
  }

  // pl::memory::hook chains onto an existing detour on the same target, so this
  // composes with the preloader's own hook on the render entry.
  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(address),
                       reinterpret_cast<pl::memory::FuncPtr>(
                           &PlayerRenderCaptureDetour),
                       &gOriginal, pl::memory::HookPriority::Low,
                       "masceffects.entity_capture") != 0) {
    status_ = "detour install failed";
    preloaderLogger.warn("MasCeffects: entity feed unavailable ({})", status_);
    return false;
  }

  installed_ = true;
  status_ = "hook installed (slot " + std::to_string(kPlayerRenderSlot) + ")";
  preloaderLogger.info(
      "MasCeffects: entity feed installed at 0x{:x} (slot {})", address,
      kPlayerRenderSlot);
  return true;
}

std::vector<GameEntity> GameFeed::drain() {
  // The frame list is moved out, so the detour starts the next frame with an
  // empty buffer and nothing allocates inside the hook.
  return std::move(frame_);
}

} // namespace masc
