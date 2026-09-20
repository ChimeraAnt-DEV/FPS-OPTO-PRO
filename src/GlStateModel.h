#pragma once

// Pure GL ES state model plus the redundancy decisions made on top of it.
//
// This header deliberately depends on nothing but the C++ standard library so
// the whole decision layer can be unit-tested on the host. It contains no
// Android, EGL or GL entry points.
//
// Correctness rules the model relies on:
//   1. Context state is owned by the context, so a shadow survives being made
//      un-current and current again. It is per context, created at the spec
//      defaults by `freshContextState()`, and is only ever exposed for a
//      context the caller has registered as one this mod saw created.
//   2. A field the spec does not pin down (viewport, program, object bindings,
//      pixel-store values) starts at an impossible sentinel, so the first call
//      of that kind is always issued and the shadow is learned from the real
//      stream.
//   3. Object names are non-zero wherever a name is required, which is what
//      makes a sentinel of `kUnknownUint` safe. Because names are recycled on
//      delete, a non-zero name is never enough to drop a call; see
//      `canDropBinding`.

#include <cstdint>
#include <limits>

namespace fpsopto {

// GLenum values from GLES2/gl2.h, GLES3/gl3.h and GLES2/gl2ext.h.
enum GlEnum : std::uint32_t {
  kGlFront = 0x0404,
  kGlBack = 0x0405,
  kGlFrontAndBack = 0x0408,

  kGlCullFace = 0x0B44,
  kGlDepthTest = 0x0B71,
  kGlDepthFunc = 0x0B74,
  kGlDepthRange = 0x0B70,
  kGlDither = 0x0BD0,
  kGlBlend = 0x0BE2,
  kGlScissorTest = 0x0C11,
  kGlStencilTest = 0x0B90,
  kGlPolygonOffsetFill = 0x8037,
  kGlSampleAlphaToCoverage = 0x809E,
  kGlSampleCoverage = 0x80A0,
  kGlSampleCoverageValue = 0x80AA,
  kGlSampleCoverageInvert = 0x80AB,
  kGlDepthWritemask = 0x0B72,
  kGlColorWritemask = 0x0C23,
  kGlCullFaceMode = 0x0B45,
  kGlFrontFace = 0x0B46,
  kGlLineWidth = 0x0B21,
  kGlPolygonOffsetUnits = 0x2A00,
  kGlPolygonOffsetFactor = 0x8038,
  kGlBlendSrcRgb = 0x80C9,
  kGlBlendDstRgb = 0x80C8,
  kGlBlendSrcAlpha = 0x80CB,
  kGlBlendDstAlpha = 0x80CA,
  kGlBlendEquationRgb = 0x8009,
  kGlBlendEquationAlpha = 0x883D,
  kGlCurrentProgram = 0x8B8D,
  kGlViewport = 0x0BA2,
  kGlUnpackAlignment = 0x0CF5,
  kGlPackAlignment = 0x0D05,
  kGlUnpackRowLength = 0x0CF2,
  kGlPixelUnpackBuffer = 0x88EC,
  kGlPixelPackBuffer = 0x88EB,

  kGlArrayBuffer = 0x8892,
  kGlElementArrayBuffer = 0x8893,
  kGlTexture2d = 0x0DE1,
  kGlTexture3d = 0x806F,
  kGlTexture2dArray = 0x8C1A,
  kGlTextureCubeMap = 0x8513,
  kGlTextureExternalOes = 0x8D65,
  kGlTextureCubeMapPositiveX = 0x8515,
  kGlTextureCubeMapNegativeX = 0x8516,
  kGlTextureCubeMapPositiveY = 0x8517,
  kGlTextureCubeMapNegativeY = 0x8518,
  kGlTextureCubeMapPositiveZ = 0x8519,
  kGlTextureCubeMapNegativeZ = 0x851A,

  kGlFramebuffer = 0x8D40,
  kGlDrawFramebuffer = 0x8CA9,
  kGlReadFramebuffer = 0x8CA8,
  kGlRenderbuffer = 0x8D41,
  kGlVertexArray = 0x85B5,

  kGlBlendEquationAdd = 0x8006,
  kGlAlways = 0x0207,
  kGlLess = 0x0201,
  kGlCw = 0x0900,
  kGlCcw = 0x0901,
  kGlTexture0 = 0x84C0,
};

// Capabilities tracked as a bitfield. Everything RenderDragon can toggle on a
// GL ES 3.x context that is cheap to shadow.
enum Capability : std::uint32_t {
  kCapBlend = 1u << 0,
  kCapCullFace = 1u << 1,
  kCapDepthTest = 1u << 2,
  kCapDither = 1u << 3,
  kCapPolygonOffsetFill = 1u << 4,
  kCapSampleAlphaToCoverage = 1u << 5,
  kCapSampleCoverage = 1u << 6,
  kCapScissorTest = 1u << 7,
  kCapStencilTest = 1u << 8,
};

// Values a real caller can never pass, used so the first call of a kind is
// always forwarded. `kUnknownFloat` is NaN, which compares unequal to itself.
inline constexpr std::uint32_t kUnknownUint = 0xFFFFFFFFu;
inline constexpr int kUnknownInt = std::numeric_limits<int>::lowest();
inline constexpr std::uint8_t kUnknownByte = 0xFFu;
inline constexpr std::uint32_t kUnknownMask = 0xFFFFFFFFu;
inline const float kUnknownFloat = std::numeric_limits<float>::quiet_NaN();

// Returns the capability bit for a GL capability enum, or 0 when the enum is
// not one this mod shadows.
constexpr std::uint32_t capabilityBit(std::uint32_t capability) {
  switch (capability) {
  case kGlBlend:
    return kCapBlend;
  case kGlCullFace:
    return kCapCullFace;
  case kGlDepthTest:
    return kCapDepthTest;
  case kGlDither:
    return kCapDither;
  case kGlPolygonOffsetFill:
    return kCapPolygonOffsetFill;
  case kGlSampleAlphaToCoverage:
    return kCapSampleAlphaToCoverage;
  case kGlSampleCoverage:
    return kCapSampleCoverage;
  case kGlScissorTest:
    return kCapScissorTest;
  case kGlStencilTest:
    return kCapStencilTest;
  default:
    return 0;
  }
}

// Index of a texture target inside `TextureUnits::textures`.
enum TextureSlot : int {
  kSlotTexture2d = 1,
  kSlotTexture3d = 2,
  kSlotTexture2dArray = 3,
  kSlotTextureCubeMap = 4,
  kSlotTextureExternalOes = 5,
  kTextureSlotCount = 6,
};

// Returns the slot a texture target binds into, or 0 for unknown targets.
constexpr int textureTargetSlot(std::uint32_t target) {
  switch (target) {
  case kGlTexture2d:
    return kSlotTexture2d;
  case kGlTexture3d:
    return kSlotTexture3d;
  case kGlTexture2dArray:
    return kSlotTexture2dArray;
  case kGlTextureCubeMap:
  case kGlTextureCubeMapPositiveX:
  case kGlTextureCubeMapNegativeX:
  case kGlTextureCubeMapPositiveY:
  case kGlTextureCubeMapNegativeY:
  case kGlTextureCubeMapPositiveZ:
  case kGlTextureCubeMapNegativeZ:
    return kSlotTextureCubeMap;
  case kGlTextureExternalOes:
    return kSlotTextureExternalOes;
  default:
    return 0;
  }
}

// Every field the filter needs to decide whether a state call is redundant.
struct GlStateModel {
  std::uint32_t enabled = 0;

  std::uint32_t blendSrcRgb = kUnknownUint;
  std::uint32_t blendDstRgb = kUnknownUint;
  std::uint32_t blendSrcAlpha = kUnknownUint;
  std::uint32_t blendDstAlpha = kUnknownUint;
  std::uint32_t blendEquationRgb = kGlBlendEquationAdd;
  std::uint32_t blendEquationAlpha = kGlBlendEquationAdd;

  std::uint32_t depthFunc = kGlLess;
  std::uint8_t depthMask = 1;

  std::uint32_t colorMaskR = 1;
  std::uint32_t colorMaskG = 1;
  std::uint32_t colorMaskB = 1;
  std::uint32_t colorMaskA = 1;

  std::uint32_t cullFaceMode = kGlBack;
  std::uint32_t frontFace = kGlCcw;

  std::uint32_t stencilFunc = 0; // 0 is not a valid test, so it never matches
  int stencilRef = kUnknownInt;
  std::uint32_t stencilValueMask = kUnknownMask;
  std::uint32_t stencilWriteMask = kUnknownMask;

  float blendColorR = 0.0f;
  float blendColorG = 0.0f;
  float blendColorB = 0.0f;
  float blendColorA = 0.0f;

  float sampleCoverageValue = 1.0f;
  std::uint8_t sampleCoverageInvert = 0;

  float lineWidth = 1.0f;
  float polygonOffsetFactor = 0.0f;
  float polygonOffsetUnits = 0.0f;

  float depthRangeNear = 0.0f;
  float depthRangeFar = 1.0f;

  // The spec ties the default viewport to the drawable size, so it is not
  // knowable ahead of time and starts unknown.
  std::int32_t viewportX = kUnknownInt;
  std::int32_t viewportY = kUnknownInt;
  std::int32_t viewportW = kUnknownInt;
  std::int32_t viewportH = kUnknownInt;

  std::uint32_t currentProgram = kUnknownUint;

  // A fresh context starts on GL_TEXTURE0 by spec, so this one is known.
  std::uint32_t activeTexture = kGlTexture0;

  std::uint32_t arrayBuffer = kUnknownUint;
  // GL_ELEMENT_ARRAY_BUFFER is deliberately absent: its binding is part of the
  // vertex-array object rather than the context, so a context-level shadow
  // cannot be kept correct. Those binds are always forwarded.
  std::uint32_t framebuffer = kUnknownUint;
  std::uint32_t vertexArray = kUnknownUint;

  std::uint32_t unpackAlignment = kUnknownUint;
  std::uint32_t packAlignment = kUnknownUint;
  std::uint32_t unpackRowLength = kUnknownUint;
  std::uint32_t pixelUnpackBuffer = kUnknownUint;
  std::uint32_t pixelPackBuffer = kUnknownUint;
};

// Per-unit texture bindings. Unlike the rest of the context state these are
// not global: two units can hold different images, and a stale entry would
// make a pass sample the wrong texture, so they are tracked per unit.
struct TextureUnits {
  // GL ES only guarantees 8 combined units; 32 covers real drivers with room
  // to spare, and the queried value is clamped to this.
  static constexpr int kMaxUnits = 32;

  std::uint32_t textures[kMaxUnits][kTextureSlotCount]{};

  // Returns the unit index for a GL_TEXTUREn enum, or -1 when out of range.
  static constexpr int unitOf(std::uint32_t activeTextureEnum) {
    const int unit = static_cast<int>(activeTextureEnum - kGlTexture0);
    return (unit >= 0 && unit < kMaxUnits) ? unit : -1;
  }
};

// State a context is required to have the moment it becomes current, per the
// GL ES 2.0/3.x specification. Only values the spec pins down are set; every
// other field keeps its sentinel so its first call is forwarded.
constexpr GlStateModel freshContextState() {
  GlStateModel state{};
  // The spec enables exactly one capability at context creation: GL_DITHER.
  // Everything else, GL_CULL_FACE included, starts disabled, and claiming
  // otherwise would drop the caller's first glEnable and change what is drawn.
  state.enabled = kCapDither;
  state.depthFunc = kGlLess;
  state.depthMask = 1;
  state.cullFaceMode = kGlBack;
  state.frontFace = kGlCcw;
  state.blendEquationRgb = kGlBlendEquationAdd;
  state.blendEquationAlpha = kGlBlendEquationAdd;
  state.activeTexture = kGlTexture0;
  return state;
}

// ---------------------------------------------------------------------------
// Redundancy decisions
// ---------------------------------------------------------------------------

constexpr bool sameBlend(const GlStateModel &state, std::uint32_t srcRgb,
                         std::uint32_t dstRgb, std::uint32_t srcAlpha,
                         std::uint32_t dstAlpha) {
  return state.blendSrcRgb == srcRgb && state.blendDstRgb == dstRgb &&
         state.blendSrcAlpha == srcAlpha && state.blendDstAlpha == dstAlpha;
}

constexpr bool sameColorMask(const GlStateModel &state, std::uint32_t r,
                             std::uint32_t g, std::uint32_t b,
                             std::uint32_t a) {
  return state.colorMaskR == r && state.colorMaskG == g &&
         state.colorMaskB == b && state.colorMaskA == a;
}

constexpr bool sameStencilFunc(const GlStateModel &state, std::uint32_t func,
                               int ref, std::uint32_t mask) {
  return state.stencilFunc == func && state.stencilRef == ref &&
         state.stencilValueMask == mask;
}

constexpr bool sameBlendColor(const GlStateModel &state, float r, float g,
                              float b, float a) {
  return state.blendColorR == r && state.blendColorG == g &&
         state.blendColorB == b && state.blendColorA == a;
}

constexpr bool sameSampleCoverage(const GlStateModel &state, float value,
                                  std::uint8_t invert) {
  return state.sampleCoverageValue == value &&
         state.sampleCoverageInvert == invert;
}

constexpr bool sameDepthRange(const GlStateModel &state, float near, float far) {
  return state.depthRangeNear == near && state.depthRangeFar == far;
}

constexpr bool sameViewport(const GlStateModel &state, int x, int y, int w,
                            int h) {
  return state.viewportX == x && state.viewportY == y &&
         state.viewportW == w && state.viewportH == h;
}

// A binding can only ever be dropped when the call is an unbind (name 0) whose
// shadow already says "unbound". A non-zero shadow proves nothing: object
// names are recycled once the object is deleted, so the name the context holds
// may no longer be the object that was bound, and a client that owns several
// contexts can hold a binding outside this shadow entirely. The "unknown"
// sentinel (0xFFFFFFFF, a name no driver issues) can therefore never be
// mistaken for a real binding, while the real binding 0 -- the default
// framebuffer, an unbind -- is correctly recognised as redundant on repeat.
constexpr bool sameBinding(std::uint32_t shadow, std::uint32_t name) {
  return shadow == name;
}

constexpr bool canDropBinding(std::uint32_t shadow, std::uint32_t name) {
  return name == 0 && sameBinding(shadow, name);
}

// The pixel-store functions take a signed value; the shadow keeps the exact
// bit pattern so a redundant set is recognised and a differing one is not.
constexpr bool samePixelStore(std::uint32_t shadow, int param) {
  return shadow == static_cast<std::uint32_t>(param);
}

} // namespace fpsopto