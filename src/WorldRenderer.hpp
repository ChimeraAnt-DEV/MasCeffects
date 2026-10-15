#pragma once

/**
 * @file WorldRenderer.hpp
 * @brief Draws world-space line geometry into the game's own frame.
 *
 * The preloader's render callback runs on the render thread with the GL context
 * current, so a mod may issue GL calls directly. This wraps that in the smallest
 * safe surface: one shader, one dynamic vertex buffer, and a strict
 * save/restore of every piece of GL state it touches.
 *
 * <p><b>Fail-closed.</b> If the GLES table is missing an entry, a shader will
 * not compile, or no vertex buffer can be created, {@link ready} stays false and
 * every draw is a no-op. The game then renders exactly as it did without the
 * mod. Nothing here ever clears or reallocates the game's own framebuffer.
 *
 * <p><b>Ownership.</b> The render thread is the only thread that may call
 * {@link draw}; the GL context belongs to it.
 */

#include <cstdint>
#include <vector>

#include <pl/cosmetics/NativeModelMatrix.hpp>
#include <pl/cosmetics/NativeCosmeticRenderer.hpp>

#include "EffectMath.hpp"

namespace masc {

// GLES entry-point signatures for the optional vertex-array functions, which
// the preloader's core table does not carry. Spelled out here so the header
// needs no GL include.
using GenVertexArraysFn = void (*)(std::int32_t, std::uint32_t *);
using BindVertexArrayFn = void (*)(std::uint32_t);
using DeleteVertexArraysFn = void (*)(std::int32_t, const std::uint32_t *);
using DeleteProgramFn = void (*)(std::uint32_t);

class WorldRenderer {
public:
  WorldRenderer() = default;
  ~WorldRenderer();

  WorldRenderer(const WorldRenderer &) = delete;
  WorldRenderer &operator=(const WorldRenderer &) = delete;

  /**
   * @brief Prepares the shader and buffers. Safe to call every frame.
   *
   * Only the render thread may call this. A failure is sticky: once the
   * pipeline is known bad it is not retried, so a broken driver cannot cost a
   * shader compile every frame.
   */
  bool EnsureReady();

  /** @brief True once the pipeline is live and {@link draw} can do work. */
  [[nodiscard]] bool ready() const { return ready_; }

  /**
   * @brief Uploads and draws @p segments as GL_LINES.
   *
   * @param viewProjection world -> clip matrix from the render context
   * @param segments       world-space line list
   * @param color          `0xAARRGGBB`; alpha is honoured via blending
   * @return true when the geometry was submitted
   */
  bool draw(const pl::cosmetics::Mat4 &viewProjection,
            const std::vector<Segment> &segments, Rgba color);

  /** @brief Releases the GL objects. Call while the context is still current. */
  void shutdown();

private:
  bool BuildProgram();
  void ApplyState();
  void RestoreState();

  pl::cosmetics::GlesFunctions gles_{};
  bool glesResolved_ = false;
  bool initFailed_ = false;
  bool ready_ = false;

  /// Optional; the draw path falls back to plain attribute state without them.
  GenVertexArraysFn genVertexArrays_ = nullptr;
  BindVertexArrayFn bindVertexArray_ = nullptr;
  DeleteVertexArraysFn deleteVertexArrays_ = nullptr;
  DeleteProgramFn deleteProgram_ = nullptr;
  bool hasVao_ = false;

  unsigned int program_ = 0;
  unsigned int vao_ = 0;
  unsigned int vbo_ = 0;
  int mvpLocation_ = -1;
  int colorLocation_ = -1;

  // Saved GL state, valid only between ApplyState() and RestoreState().
  std::int32_t savedProgram_ = 0;
  std::int32_t savedVao_ = 0;
  std::int32_t savedArrayBuffer_ = 0;
  std::int32_t savedDepthTest_ = 0;
  std::int32_t savedBlend_ = 0;
  std::int32_t savedDepthMask_ = 0;
  std::int32_t savedBlendSrc_ = 0;
  std::int32_t savedBlendDst_ = 0;

  /// Interleaved xyz floats; kept as a member so a draw never allocates.
  std::vector<float> vertices_;
};

} // namespace masc
