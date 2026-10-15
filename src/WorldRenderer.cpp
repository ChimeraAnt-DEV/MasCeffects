#include "WorldRenderer.hpp"

#include <cstring>

#include <dlfcn.h>

#include <pl/Logger.hpp>

namespace masc {
namespace {

using namespace pl::cosmetics;

// GLES 2.0 constants. They are spelled out rather than pulled from a GL header
// because the preloader resolves GLES by dlsym and never links libGLESv2, so the
// mod must not link it either.
constexpr std::uint32_t kGlVertexShader = 0x8B31;
constexpr std::uint32_t kGlFragmentShader = 0x8B30;
constexpr std::uint32_t kGlCompileStatus = 0x8B81;
constexpr std::uint32_t kGlLinkStatus = 0x8B82;
constexpr std::uint32_t kGlArrayBuffer = 0x8892;
constexpr std::uint32_t kGlArrayBufferBinding = 0x8894;
constexpr std::uint32_t kGlCurrentProgram = 0x8B8D;
constexpr std::uint32_t kGlVertexArrayBinding = 0x85B5;
constexpr std::uint32_t kGlDepthTest = 0x0B71;
constexpr std::uint32_t kGlBlend = 0x0BE2;
constexpr std::uint32_t kGlDepthWritemask = 0x0B72;
constexpr std::uint32_t kGlBlendSrcRgb = 0x0C02;
constexpr std::uint32_t kGlBlendDstRgb = 0x0C01;
constexpr std::uint32_t kGlSrcAlpha = 0x0302;
constexpr std::uint32_t kGlOneMinusSrcAlpha = 0x0303;
constexpr std::uint32_t kGlFloat = 0x1406;
constexpr std::uint32_t kGlLines = 0x0001;
constexpr std::uint32_t kGlDynamicDraw = 0x88E8;

constexpr const char *kVertexShader = R"(#version 100
attribute vec3 aPosition;
uniform mat4 uMvp;
void main() {
  gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

constexpr const char *kFragmentShader = R"(#version 100
precision mediump float;
uniform vec4 uColor;
void main() {
  gl_FragColor = uColor;
}
)";

using GetIntegervFn = void (*)(std::uint32_t, std::int32_t *);
using GetBooleanvFn = void (*)(std::uint32_t, unsigned char *);
using CreateShaderFn = std::uint32_t (*)(std::uint32_t);
using ShaderSourceFn = void (*)(std::uint32_t, std::int32_t,
                                const char *const *, const std::int32_t *);
using CompileShaderFn = void (*)(std::uint32_t);
using GetShaderivFn = void (*)(std::uint32_t, std::uint32_t, std::int32_t *);
using CreateProgramFn = std::uint32_t (*)();
using AttachShaderFn = void (*)(std::uint32_t, std::uint32_t);
using LinkProgramFn = void (*)(std::uint32_t);
using GetProgramivFn = void (*)(std::uint32_t, std::uint32_t, std::int32_t *);
using DeleteShaderFn = void (*)(std::uint32_t);
using UseProgramFn = void (*)(std::uint32_t);
using GetUniformLocationFn = int (*)(std::uint32_t, const char *);
using UniformMatrix4fvFn = void (*)(int, std::int32_t, unsigned char,
                                    const float *);
using Uniform4fFn = void (*)(int, float, float, float, float);
using GenBuffersFn = void (*)(std::int32_t, std::uint32_t *);
using BindBufferFn = void (*)(std::uint32_t, std::uint32_t);
using BufferDataFn = void (*)(std::uint32_t, std::intptr_t, const void *,
                              std::uint32_t);
using EnableVertexAttribArrayFn = void (*)(std::uint32_t);
using VertexAttribPointerFn = void (*)(std::uint32_t, std::int32_t,
                                       std::uint32_t, unsigned char,
                                       std::int32_t, const void *);
using DrawArraysFn = void (*)(std::uint32_t, std::int32_t, std::int32_t);
using EnableFn = void (*)(std::uint32_t);
using DisableFn = void (*)(std::uint32_t);
using DepthMaskFn = void (*)(unsigned char);
using BlendFuncFn = void (*)(std::uint32_t, std::uint32_t);
using DeleteBuffersFn = void (*)(std::int32_t, const std::uint32_t *);

/// Reinterprets a resolved GL entry point. The preloader hands back `void *`
/// because the table is built by dlsym, so the cast is unavoidable; every
/// signature above is the GLES 2.0 one.
template <typename Fn> Fn As(void *symbol) {
  return reinterpret_cast<Fn>(symbol);
}

/// Resolves a GL entry point from libGLESv2. Used only for the optional vertex
/// array functions, which the preloader's core table does not carry.
template <typename Fn> Fn ResolveGlesSymbol(const char *name) {
  void *symbol = dlsym(RTLD_DEFAULT, name);
  if (symbol == nullptr) {
    void *library = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
    if (library != nullptr) symbol = dlsym(library, name);
  }
  return reinterpret_cast<Fn>(symbol);
}

float AlphaOf(Rgba color) {
  return static_cast<float>((color >> 24) & 0xFFu) / 255.0F;
}

float RedOf(Rgba color) {
  return static_cast<float>((color >> 16) & 0xFFu) / 255.0F;
}

float GreenOf(Rgba color) {
  return static_cast<float>((color >> 8) & 0xFFu) / 255.0F;
}

float BlueOf(Rgba color) {
  return static_cast<float>(color & 0xFFu) / 255.0F;
}

} // namespace

WorldRenderer::~WorldRenderer() { shutdown(); }

bool WorldRenderer::EnsureReady() {
  if (ready_) return true;
  if (initFailed_) return false;

  if (!glesResolved_) {
    gles_ = ResolveGles();
    glesResolved_ = true;
  }
  if (!gles_.usable() || !gles_.GetBooleanv || !gles_.BlendFunc) {
    initFailed_ = true;
    preloaderLogger.warn(
        "MasCeffects: GLES table incomplete; world effects disabled");
    return false;
  }

  // Vertex arrays are optional: without them the attribute pointer is re-sent
  // per draw, which is correct on every GLES 2.0 driver.
  genVertexArrays_ = ResolveGlesSymbol<GenVertexArraysFn>("glGenVertexArrays");
  bindVertexArray_ = ResolveGlesSymbol<BindVertexArrayFn>("glBindVertexArray");
  deleteVertexArrays_ =
      ResolveGlesSymbol<DeleteVertexArraysFn>("glDeleteVertexArrays");
  // The core table carries no glDeleteProgram, so it is resolved the same way.
  deleteProgram_ = ResolveGlesSymbol<DeleteProgramFn>("glDeleteProgram");
  hasVao_ = genVertexArrays_ != nullptr && bindVertexArray_ != nullptr;

  if (!BuildProgram()) {
    initFailed_ = true;
    return false;
  }
  ready_ = true;
  preloaderLogger.info("MasCeffects: world render pipeline ready (vao={})",
                           hasVao_);
  return true;
}

bool WorldRenderer::BuildProgram() {
  const std::uint32_t vertex =
      As<CreateShaderFn>(gles_.CreateShader)(kGlVertexShader);
  const std::uint32_t fragment =
      As<CreateShaderFn>(gles_.CreateShader)(kGlFragmentShader);
  if (vertex == 0 || fragment == 0) return false;

  auto compile = [this](std::uint32_t shader, const char *source) {
    const auto length = static_cast<std::int32_t>(std::strlen(source));
    const char *sources[1] = {source};
    As<ShaderSourceFn>(gles_.ShaderSource)(shader, 1, sources, &length);
    As<CompileShaderFn>(gles_.CompileShader)(shader);
    std::int32_t status = 0;
    As<GetShaderivFn>(gles_.GetShaderiv)(shader, kGlCompileStatus, &status);
    return status != 0;
  };

  if (!compile(vertex, kVertexShader) || !compile(fragment, kFragmentShader)) {
    As<DeleteShaderFn>(gles_.DeleteShader)(vertex);
    As<DeleteShaderFn>(gles_.DeleteShader)(fragment);
    preloaderLogger.warn("MasCeffects: shader compilation failed");
    return false;
  }

  const std::uint32_t program = As<CreateProgramFn>(gles_.CreateProgram)();
  if (program == 0) {
    As<DeleteShaderFn>(gles_.DeleteShader)(vertex);
    As<DeleteShaderFn>(gles_.DeleteShader)(fragment);
    return false;
  }
  As<AttachShaderFn>(gles_.AttachShader)(program, vertex);
  As<AttachShaderFn>(gles_.AttachShader)(program, fragment);
  As<LinkProgramFn>(gles_.LinkProgram)(program);

  std::int32_t linked = 0;
  As<GetProgramivFn>(gles_.GetProgramiv)(program, kGlLinkStatus, &linked);
  // The shaders are owned by the program once attached, so they are always
  // flagged for deletion; the link result decides whether the program is kept.
  As<DeleteShaderFn>(gles_.DeleteShader)(vertex);
  As<DeleteShaderFn>(gles_.DeleteShader)(fragment);
  if (linked == 0) {
    if (deleteProgram_ != nullptr) deleteProgram_(program);
    preloaderLogger.warn("MasCeffects: shader program link failed");
    return false;
  }

  program_ = program;
  mvpLocation_ =
      As<GetUniformLocationFn>(gles_.GetUniformLocation)(program_, "uMvp");
  colorLocation_ =
      As<GetUniformLocationFn>(gles_.GetUniformLocation)(program_, "uColor");
  if (mvpLocation_ < 0 || colorLocation_ < 0) {
    shutdown();
    return false;
  }

  As<GenBuffersFn>(gles_.GenBuffers)(1, &vbo_);
  if (vbo_ == 0) {
    shutdown();
    return false;
  }
  if (hasVao_) {
    genVertexArrays_(1, &vao_);
    if (vao_ == 0) {
      shutdown();
      return false;
    }
    // The attribute binding lives in the VAO, so it is set up once here and the
    // per-frame path only rebinds the VAO and refills the buffer.
    bindVertexArray_(vao_);
    As<BindBufferFn>(gles_.BindBuffer)(kGlArrayBuffer, vbo_);
    As<EnableVertexAttribArrayFn>(gles_.EnableVertexAttribArray)(0);
    As<VertexAttribPointerFn>(gles_.VertexAttribPointer)(
        0, 3, kGlFloat, 0, static_cast<std::int32_t>(3 * sizeof(float)),
        nullptr);
    bindVertexArray_(0);
    As<BindBufferFn>(gles_.BindBuffer)(kGlArrayBuffer, 0);
  }
  return true;
}

void WorldRenderer::ApplyState() {
  auto getInt = As<GetIntegervFn>(gles_.GetIntegerv);
  getInt(kGlCurrentProgram, &savedProgram_);
  if (hasVao_) getInt(kGlVertexArrayBinding, &savedVao_);
  getInt(kGlArrayBufferBinding, &savedArrayBuffer_);
  getInt(kGlDepthTest, &savedDepthTest_);
  getInt(kGlBlend, &savedBlend_);
  getInt(kGlBlendSrcRgb, &savedBlendSrc_);
  getInt(kGlBlendDstRgb, &savedBlendDst_);

  unsigned char depthMask = 1;
  As<GetBooleanvFn>(gles_.GetBooleanv)(kGlDepthWritemask, &depthMask);
  savedDepthMask_ = depthMask;

  As<UseProgramFn>(gles_.UseProgram)(program_);
  if (hasVao_) {
    bindVertexArray_(vao_);
  } else {
    As<BindBufferFn>(gles_.BindBuffer)(kGlArrayBuffer, vbo_);
    As<EnableVertexAttribArrayFn>(gles_.EnableVertexAttribArray)(0);
    As<VertexAttribPointerFn>(gles_.VertexAttribPointer)(
        0, 3, kGlFloat, 0, static_cast<std::int32_t>(3 * sizeof(float)),
        nullptr);
  }
  As<EnableFn>(gles_.Enable)(kGlBlend);
  As<BlendFuncFn>(gles_.BlendFunc)(kGlSrcAlpha, kGlOneMinusSrcAlpha);
  // Effects are drawn against the already-rendered world, so they must not
  // write depth.
  As<DisableFn>(gles_.Disable)(kGlDepthTest);
  As<DepthMaskFn>(gles_.DepthMask)(0);
}

void WorldRenderer::RestoreState() {
  As<DepthMaskFn>(gles_.DepthMask)(static_cast<unsigned char>(savedDepthMask_));
  if (savedDepthTest_ != 0) {
    As<EnableFn>(gles_.Enable)(kGlDepthTest);
  } else {
    As<DisableFn>(gles_.Disable)(kGlDepthTest);
  }
  if (savedBlend_ != 0) {
    As<EnableFn>(gles_.Enable)(kGlBlend);
  } else {
    As<DisableFn>(gles_.Disable)(kGlBlend);
  }
  As<BlendFuncFn>(gles_.BlendFunc)(static_cast<std::uint32_t>(savedBlendSrc_),
                                   static_cast<std::uint32_t>(savedBlendDst_));
  if (hasVao_) {
    bindVertexArray_(static_cast<std::uint32_t>(savedVao_));
  } else {
    As<BindBufferFn>(gles_.BindBuffer)(
        kGlArrayBuffer, static_cast<std::uint32_t>(savedArrayBuffer_));
  }
  As<UseProgramFn>(gles_.UseProgram)(static_cast<std::uint32_t>(savedProgram_));
}

bool WorldRenderer::draw(const Mat4 &viewProjection,
                         const std::vector<Segment> &segments, Rgba color) {
  if (!ready_ || segments.empty()) return false;
  if (AlphaOf(color) <= 0.0F) return false;

  vertices_.clear();
  vertices_.reserve(segments.size() * 6);
  for (const Segment &s : segments) {
    vertices_.insert(vertices_.end(), {s.x0, s.y0, s.z0, s.x1, s.y1, s.z1});
  }

  ApplyState();
  As<BindBufferFn>(gles_.BindBuffer)(kGlArrayBuffer, vbo_);
  As<BufferDataFn>(gles_.BufferData)(
      kGlArrayBuffer,
      static_cast<std::intptr_t>(vertices_.size() * sizeof(float)),
      vertices_.data(), kGlDynamicDraw);

  As<UniformMatrix4fvFn>(gles_.UniformMatrix4fv)(mvpLocation_, 1, 0,
                                                 viewProjection.m);
  As<Uniform4fFn>(gles_.Uniform4f)(colorLocation_, RedOf(color), GreenOf(color),
                                   BlueOf(color), AlphaOf(color));

  const auto vertexCount = static_cast<std::int32_t>(vertices_.size() / 3);
  As<DrawArraysFn>(gles_.DrawArrays)(kGlLines, 0, vertexCount);

  RestoreState();
  return true;
}

void WorldRenderer::shutdown() {
  if (!glesResolved_) return;
  if (gles_.DeleteBuffers && vbo_ != 0) {
    As<DeleteBuffersFn>(gles_.DeleteBuffers)(1, &vbo_);
  }
  if (deleteVertexArrays_ != nullptr && vao_ != 0) {
    deleteVertexArrays_(1, &vao_);
  }
  if (deleteProgram_ != nullptr && program_ != 0) {
    deleteProgram_(program_);
  }
  vbo_ = 0;
  vao_ = 0;
  program_ = 0;
  ready_ = false;
}

} // namespace masc
