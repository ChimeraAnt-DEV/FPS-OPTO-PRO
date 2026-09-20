// Verifies the interposer's central promise: a call is only dropped when the
// driver would have ignored it, and every state-changing call the game makes
// still reaches the driver in some form.
//
// The probes below stand in for the driver's entry points. GlInterceptor.cpp
// is compiled with PL_INTERPOSER_HOST_TEST, which turns its call-through names
// into references to exactly these probe functions, so the assertions below
// observe the same stream a real driver would.

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "GlInterceptor.h"
#include "test_support.h"

namespace {

struct Trace {
  std::vector<std::string> calls;
  void reset() { calls.clear(); }
  bool empty() const { return calls.empty(); }
  std::size_t count(const char *name) const {
    std::size_t total = 0;
    for (const auto &call : calls) {
      if (call == name) {
        ++total;
      }
    }
    return total;
  }
};

Trace gTrace;

void note(const char *name) { gTrace.calls.emplace_back(name); }

} // namespace

// ---------------------------------------------------------------------------
// Probe entry points, replacing the real GL ES exports under test
// ---------------------------------------------------------------------------

extern "C" {
void glEnable(unsigned int) { note("glEnable"); }
void glDisable(unsigned int) { note("glDisable"); }
void glBlendFunc(unsigned int, unsigned int) { note("glBlendFunc"); }
void glBlendFuncSeparate(unsigned int, unsigned int, unsigned int, unsigned int) {
  note("glBlendFuncSeparate");
}
void glBlendEquation(unsigned int) { note("glBlendEquation"); }
void glBlendEquationSeparate(unsigned int, unsigned int) {
  note("glBlendEquationSeparate");
}
void glBlendColor(float, float, float, float) { note("glBlendColor"); }
void glDepthFunc(unsigned int) { note("glDepthFunc"); }
void glDepthMask(unsigned char) { note("glDepthMask"); }
void glColorMask(unsigned char, unsigned char, unsigned char, unsigned char) {
  note("glColorMask");
}
void glCullFace(unsigned int) { note("glCullFace"); }
void glFrontFace(unsigned int) { note("glFrontFace"); }
void glDepthRangef(float, float) { note("glDepthRangef"); }
void glSampleCoverage(float, unsigned char) { note("glSampleCoverage"); }
void glViewport(int, int, int, int) { note("glViewport"); }
void glUseProgram(unsigned int) { note("glUseProgram"); }
void glActiveTexture(unsigned int) { note("glActiveTexture"); }
void glBindBuffer(unsigned int, unsigned int) { note("glBindBuffer"); }
void glBindTexture(unsigned int, unsigned int) { note("glBindTexture"); }
void glBindFramebuffer(unsigned int, unsigned int) { note("glBindFramebuffer"); }
void glBindVertexArray(unsigned int) { note("glBindVertexArray"); }
void glPixelStorei(unsigned int, int) { note("glPixelStorei"); }
}

// The interposer's own detours, reused directly as the caller under test.
extern "C" {
void fpsopto_glEnable(unsigned int);
void fpsopto_glDisable(unsigned int);
void fpsopto_glBlendFunc(unsigned int, unsigned int);
void fpsopto_glBlendFuncSeparate(unsigned int, unsigned int, unsigned int,
                                 unsigned int);
void fpsopto_glBlendEquation(unsigned int);
void fpsopto_glBlendColor(float, float, float, float);
void fpsopto_glDepthFunc(unsigned int);
void fpsopto_glDepthMask(unsigned char);
void fpsopto_glColorMask(unsigned char, unsigned char, unsigned char, unsigned char);
void fpsopto_glCullFace(unsigned int);
void fpsopto_glViewport(int, int, int, int);
void fpsopto_glUseProgram(unsigned int);
void fpsopto_glActiveTexture(unsigned int);
void fpsopto_glBindBuffer(unsigned int, unsigned int);
void fpsopto_glBindTexture(unsigned int, unsigned int);
void fpsopto_glBindFramebuffer(unsigned int, unsigned int);
void fpsopto_glBindVertexArray(unsigned int);
void fpsopto_glPixelStorei(unsigned int, int);
void fpsopto_glSampleCoverage(float, unsigned char);
void fpsopto_glDepthRangef(float, float);
void fpsopto_glFrontFace(unsigned int);
void fpsopto_glBlendEquationSeparate(unsigned int, unsigned int);
}

namespace {

void freshContext() {
  fpsopto::GlInterceptor::instance().onContextChanged();
  gTrace.reset();
}

// A short name for the interposer instance the tests drive.
fpsopto::GlInterceptor &gl() { return fpsopto::GlInterceptor::instance(); }

// ---------------------------------------------------------------------------
// Redundant state
// ---------------------------------------------------------------------------

void repeatedEnableIsDropped() {
  freshContext();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});
}

// Disabling a capability that is already off is redundant too, so the very
// first such call never reaches the driver.
void redundantDisableIsDropped() {
  freshContext();
  fpsopto_glDisable(fpsopto::kGlDepthTest);
  fpsopto_glDisable(fpsopto::kGlDepthTest);
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{0});
}

// For a capability the context starts with enabled, the first disable is real
// and a repeat of it is not. GL_DITHER is the only one the spec starts enabled.
void disablingAnEnabledCapabilityIsForwardedOnce() {
  freshContext();
  fpsopto_glDisable(fpsopto::kGlDither);
  fpsopto_glDisable(fpsopto::kGlDither);
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{1});
}

// The context starts with GL_DITHER enabled, so re-enabling it must not reach
// the driver. GL_CULL_FACE starts disabled, so enabling it must.
void specDefaultCapabilitiesAreHonoured() {
  freshContext();
  fpsopto_glEnable(fpsopto::kGlDither);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{0});

  fpsopto_glEnable(fpsopto::kGlCullFace);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});
}

// ...but disabling it is a real change and must be forwarded.
void disablingASpecDefaultIsForwarded() {
  freshContext();
  fpsopto_glDisable(fpsopto::kGlDither);
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{1});
}

void toggleThenRepeatIsDropped() {
  freshContext();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glDisable(fpsopto::kGlBlend);
  fpsopto_glDisable(fpsopto::kGlBlend);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{1});
}

void unknownCapabilityIsAlwaysForwarded() {
  freshContext();
  constexpr unsigned int kNotTracked = 0x0B70; // GL_DEPTH_RANGE, not a cap
  fpsopto_glEnable(kNotTracked);
  fpsopto_glEnable(kNotTracked);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{2});
}

void repeatedBlendFuncIsDropped() {
  freshContext();
  fpsopto_glBlendFunc(0x0302, 0x0303);
  fpsopto_glBlendFunc(0x0302, 0x0303);
  EXPECT_EQ(gTrace.count("glBlendFunc"), std::size_t{1});
}

void changedBlendFuncIsForwarded() {
  freshContext();
  fpsopto_glBlendFunc(0x0302, 0x0303);
  fpsopto_glBlendFunc(0x0302, 0x0303); // dropped
  fpsopto_glBlendFunc(0x0001, 0x0000); // different, must go through
  EXPECT_EQ(gTrace.count("glBlendFunc"), std::size_t{2});
}

// glBlendFunc sets all four modes; a following separate call that agrees with
// the four values is redundant, and one that disagrees is not.
void blendFuncThenAgreeingSeparateIsDropped() {
  freshContext();
  fpsopto_glBlendFunc(0x0302, 0x0303);
  fpsopto_glBlendFuncSeparate(0x0302, 0x0303, 0x0302, 0x0303);
  EXPECT_EQ(gTrace.count("glBlendFuncSeparate"), std::size_t{0});
}

void blendFuncThenDifferingSeparateIsForwarded() {
  freshContext();
  fpsopto_glBlendFunc(0x0302, 0x0303);
  fpsopto_glBlendFuncSeparate(0x0302, 0x0303, 0x0001, 0x0000);
  EXPECT_EQ(gTrace.count("glBlendFuncSeparate"), std::size_t{1});
}

void blendEquationTracksBothChannels() {
  freshContext();
  // GL_FUNC_ADD is the spec default, so this one is already redundant...
  fpsopto_glBlendEquation(0x8006);
  // ...and the separate call below changes only the alpha channel.
  fpsopto_glBlendEquationSeparate(0x8006, 0x800A);
  fpsopto_glBlendEquationSeparate(0x8006, 0x800A);
  EXPECT_EQ(gTrace.count("glBlendEquation"), std::size_t{0});
  EXPECT_EQ(gTrace.count("glBlendEquationSeparate"), std::size_t{1});
}

void repeatedViewportIsDroppedButAChangeIsNot() {
  freshContext();
  // The viewport's default depends on the drawable size, so the first call of
  // the run must reach the driver even though the values repeat afterwards.
  fpsopto_glViewport(0, 0, 1920, 1080);
  fpsopto_glViewport(0, 0, 1920, 1080);
  fpsopto_glViewport(0, 0, 1920, 1080);
  fpsopto_glViewport(0, 0, 1280, 720);
  EXPECT_EQ(gTrace.count("glViewport"), std::size_t{2});
}

void repeatedDepthMaskIsDropped() {
  freshContext();
  // 1 is the spec default, so the first call is already redundant and only
  // the two transitions reach the driver.
  fpsopto_glDepthMask(1);
  fpsopto_glDepthMask(0);
  fpsopto_glDepthMask(0);
  fpsopto_glDepthMask(1);
  EXPECT_EQ(gTrace.count("glDepthMask"), std::size_t{2});
}

void nonZeroDepthMaskIsNormalisedToOne() {
  freshContext();
  fpsopto_glDepthMask(0);
  // GL treats any non-zero as enabling; the shadow must not treat 0x7F as a
  // distinct value and forward calls the driver would ignore.
  fpsopto_glDepthMask(0x7F);
  fpsopto_glDepthMask(0xFF);
  fpsopto_glDepthMask(1);
  EXPECT_EQ(gTrace.count("glDepthMask"), std::size_t{2});
}

void repeatedColorMaskIsDropped() {
  freshContext();
  // All-ones is the spec default, so the first call is already redundant.
  fpsopto_glColorMask(1, 1, 1, 1);
  fpsopto_glColorMask(1, 0, 1, 1);
  fpsopto_glColorMask(1, 0, 1, 1);
  EXPECT_EQ(gTrace.count("glColorMask"), std::size_t{1});
}

void repeatedCullFaceIsDropped() {
  freshContext();
  // GL_BACK is the spec default, so the first call is already redundant.
  fpsopto_glCullFace(fpsopto::kGlBack);
  fpsopto_glCullFace(fpsopto::kGlFront);
  fpsopto_glCullFace(fpsopto::kGlFront);
  EXPECT_EQ(gTrace.count("glCullFace"), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Object bindings
// ---------------------------------------------------------------------------

void repeatedProgramBindIsDropped() {
  freshContext();
  fpsopto_glUseProgram(7);
  fpsopto_glUseProgram(7);
  fpsopto_glUseProgram(9);
  EXPECT_EQ(gTrace.count("glUseProgram"), std::size_t{2});
}

// Program 0 means "no program". A sentinel-based shadow must not mistake the
// first real bind for the initial state.
void firstProgramBindIsForwarded() {
  freshContext();
  fpsopto_glUseProgram(1);
  EXPECT_EQ(gTrace.count("glUseProgram"), std::size_t{1});
}

void repeatedBufferBindIsDropped() {
  freshContext();
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 12);
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 12);
  fpsopto_glBindBuffer(fpsopto::kGlElementArrayBuffer, 12);
  fpsopto_glBindBuffer(fpsopto::kGlElementArrayBuffer, 12);
  EXPECT_EQ(gTrace.count("glBindBuffer"), std::size_t{2});
}

// Object name 0 is a real binding (unbind / default framebuffer), and the
// "unknown" sentinel is a name no driver issues, so a repeated unbind is
// correctly recognised as redundant while the first one is not.
void repeatedUnbindIsDropped() {
  freshContext();
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 4);
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 0);
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 0);
  EXPECT_EQ(gTrace.count("glBindBuffer"), std::size_t{2});
}

void firstUnbindIsStillForwarded() {
  freshContext();
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 0);
  EXPECT_EQ(gTrace.count("glBindBuffer"), std::size_t{1});
}

void repeatedFramebufferBindIsDropped() {
  freshContext();
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 3);
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 3);
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{1});
}

// Binding the default framebuffer twice is the common end-of-frame call and is
// safe to drop, because 0 is a real name for the default framebuffer.
void repeatedDefaultFramebufferBindIsDropped() {
  freshContext();
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0);
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0);
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{1});
}

void repeatedVertexArrayBindIsDropped() {
  freshContext();
  fpsopto_glBindVertexArray(5);
  fpsopto_glBindVertexArray(5);
  EXPECT_EQ(gTrace.count("glBindVertexArray"), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Texture bindings, which are per unit
// ---------------------------------------------------------------------------

void repeatedTextureBindSameUnitIsDropped() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{1});
}

// The scenario that makes per-unit tracking necessary: the same texture name
// bound on two different units is two real binds, not one redundant one.
void sameTextureOnDifferentUnitsIsNotRedundant() {
  freshContext();
  fpsopto_glActiveTexture(fpsopto::kGlTexture0);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glActiveTexture(fpsopto::kGlTexture0 + 1);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{2});
}

void differentTexturesOnSameUnitAreForwarded() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 11);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{2});
}

void cubeMapFacesAndTargetShareTheCubeSlot() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTextureCubeMap, 20);
  fpsopto_glBindTexture(fpsopto::kGlTextureCubeMapPositiveX, 20);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{1});
}

// Binding texture 0 is a real unbind, so the transition to it is forwarded and
// a repeat of it is redundant.
void unbindingTextureIsDroppedOnRepeat() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 0);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 0);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{2});
}

// An out-of-range unit is not tracked, so its binds must all be forwarded
// rather than being attributed to the wrong unit.
void bindsOnUntrackedUnitsAreForwarded() {
  freshContext();
  // GL_TEXTURE0 + 40 is outside the tracked range.
  fpsopto_glActiveTexture(fpsopto::kGlTexture0 + 40);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{2});
}

void repeatedActiveTextureIsDropped() {
  freshContext();
  // GL_TEXTURE0 is the spec default, so this first call is already redundant.
  fpsopto_glActiveTexture(fpsopto::kGlTexture0);
  fpsopto_glActiveTexture(fpsopto::kGlTexture0);
  EXPECT_EQ(gTrace.count("glActiveTexture"), std::size_t{0});
  fpsopto_glActiveTexture(fpsopto::kGlTexture0 + 2);
  EXPECT_EQ(gTrace.count("glActiveTexture"), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Pixel store
// ---------------------------------------------------------------------------

void repeatedPixelStoreIsDropped() {
  freshContext();
  fpsopto_glPixelStorei(fpsopto::kGlUnpackAlignment, 4);
  fpsopto_glPixelStorei(fpsopto::kGlUnpackAlignment, 4);
  fpsopto_glPixelStorei(fpsopto::kGlUnpackAlignment, 1);
  EXPECT_EQ(gTrace.count("glPixelStorei"), std::size_t{2});
}

void unknownPixelStorePnameIsForwarded() {
  freshContext();
  constexpr unsigned int kUnknown = 0x0CF3; // GL_PACK_ROW_LENGTH-ish, untracked
  fpsopto_glPixelStorei(kUnknown, 4);
  fpsopto_glPixelStorei(kUnknown, 4);
  EXPECT_EQ(gTrace.count("glPixelStorei"), std::size_t{2});
}

// ---------------------------------------------------------------------------
// Context changes
// ---------------------------------------------------------------------------

// A brand new context starts at the spec defaults, so a call matching one of
// those defaults is redundant again even though it was forwarded in the
// previous context.
void contextResetRestoresSpecDefaults() {
  freshContext();
  fpsopto_glDisable(fpsopto::kGlDither); // started on: real, forwarded
  fpsopto_glDisable(fpsopto::kGlDither); // already off: dropped
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{1});

  freshContext();
  fpsopto_glDisable(fpsopto::kGlDither); // real again in the new context
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{1});
}

// A context change must forget the texture bindings: a fresh context inherits
// none, so the same name has to be issued again rather than being assumed.
void contextResetForgetsTextureBindings() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10); // redundant
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{1});

  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10); // must be issued again
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10); // redundant again
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{1});
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void statisticsCountSuppressions() {
  freshContext();
  const auto before = gl().stats();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend); // suppressed
  fpsopto_glViewport(0, 0, 1, 1);
  const auto after = gl().stats();
  EXPECT_EQ(after.stateCalls - before.stateCalls, std::uint64_t{3});
  EXPECT_EQ(after.suppressed - before.suppressed, std::uint64_t{1});
}

} // namespace

void runGlFilterTests() {
  repeatedEnableIsDropped();
  redundantDisableIsDropped();
  disablingAnEnabledCapabilityIsForwardedOnce();
  specDefaultCapabilitiesAreHonoured();
  disablingASpecDefaultIsForwarded();
  toggleThenRepeatIsDropped();
  unknownCapabilityIsAlwaysForwarded();
  repeatedBlendFuncIsDropped();
  changedBlendFuncIsForwarded();
  blendFuncThenAgreeingSeparateIsDropped();
  blendFuncThenDifferingSeparateIsForwarded();
  blendEquationTracksBothChannels();
  repeatedViewportIsDroppedButAChangeIsNot();
  repeatedDepthMaskIsDropped();
  nonZeroDepthMaskIsNormalisedToOne();
  repeatedColorMaskIsDropped();
  repeatedCullFaceIsDropped();
  repeatedProgramBindIsDropped();
  firstProgramBindIsForwarded();
  repeatedBufferBindIsDropped();
  repeatedUnbindIsDropped();
  firstUnbindIsStillForwarded();
  repeatedFramebufferBindIsDropped();
  repeatedDefaultFramebufferBindIsDropped();
  repeatedVertexArrayBindIsDropped();
  repeatedTextureBindSameUnitIsDropped();
  sameTextureOnDifferentUnitsIsNotRedundant();
  differentTexturesOnSameUnitAreForwarded();
  cubeMapFacesAndTargetShareTheCubeSlot();
  unbindingTextureIsDroppedOnRepeat();
  bindsOnUntrackedUnitsAreForwarded();
  repeatedActiveTextureIsDropped();
  repeatedPixelStoreIsDropped();
  unknownPixelStorePnameIsForwarded();
  contextResetRestoresSpecDefaults();
  contextResetForgetsTextureBindings();
  statisticsCountSuppressions();
}