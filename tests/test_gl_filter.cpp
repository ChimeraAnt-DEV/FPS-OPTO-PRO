// Verifies the interposer's central promise: a call is only dropped when the
// driver would have ignored it, and every state-changing call the game makes
// still reaches the driver in some form.
//
// The probes below stand in for the driver's entry points. GlInterceptor.cpp
// is compiled with PL_INTERPOSER_HOST_TEST, which turns its call-through names
// into references to exactly these probe functions, so the assertions below
// observe the same stream a real driver would.

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GlInterceptor.h"
#include "test_support.h"

namespace {

struct Trace {
  std::vector<std::string> calls;
  void reset() {
    std::lock_guard lock(mMutex);
    calls.clear();
  }
  bool empty() const { return calls.empty(); }
  std::size_t count(const char *name) const {
    std::lock_guard lock(mMutex);
    std::size_t total = 0;
    for (const auto &call : calls) {
      if (call == name) {
        ++total;
      }
    }
    return total;
  }
  void push(const char *name) {
    std::lock_guard lock(mMutex);
    calls.emplace_back(name);
  }

private:
  mutable std::mutex mMutex;
};

Trace gTrace;

void note(const char *name) { gTrace.push(name); }

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

// Each tracked context has its own shadow, so the tests register a fresh one
// and make it current. The returned handle is used to prove that one thread's
// context shadow is untouched by another thread making its own context current.
std::uintptr_t gNextContext = 0;

std::uintptr_t makeContext() {
  const std::uintptr_t handle = ++gNextContext;
  fpsopto::GlInterceptor::instance().noteContextCreated(handle);
  fpsopto::GlInterceptor::instance().setCurrentContextForTest(handle);
  gTrace.reset();
  return handle;
}

// A context this mod never registered: it must be forwarded unfiltered.
void makeUntrackedContext() {
  fpsopto::GlInterceptor::instance().setCurrentContextForTest(++gNextContext);
  gTrace.reset();
}

void freshContext() { (void)makeContext(); }

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

// A non-zero name is never dropped. Names are recycled once an object is
// deleted, so a matching shadow proves nothing about what the context holds.
void repeatedProgramBindIsForwarded() {
  freshContext();
  fpsopto_glUseProgram(7);
  fpsopto_glUseProgram(7);
  fpsopto_glUseProgram(9);
  EXPECT_EQ(gTrace.count("glUseProgram"), std::size_t{3});
}

// Unbinding a program that the shadow already knows is unbound is the one
// binding call that is safe to drop.
void repeatedProgramUnbindIsDropped() {
  freshContext();
  fpsopto_glUseProgram(7);
  fpsopto_glUseProgram(0);
  fpsopto_glUseProgram(0);
  EXPECT_EQ(gTrace.count("glUseProgram"), std::size_t{2});
}

// Program 0 means "no program". A sentinel-based shadow must not mistake the
// first real bind for the initial state.
void firstProgramBindIsForwarded() {
  freshContext();
  fpsopto_glUseProgram(1);
  EXPECT_EQ(gTrace.count("glUseProgram"), std::size_t{1});
}

// Every GL_ELEMENT_ARRAY_BUFFER bind is forwarded: its binding state lives in
// the current vertex-array object, not the context, so it is never shadowed.
// Only the repeat of an unbind on a context-level target may be dropped.
void elementArrayBufferBindsAreAlwaysForwarded() {
  freshContext();
  fpsopto_glBindBuffer(fpsopto::kGlElementArrayBuffer, 12);
  fpsopto_glBindBuffer(fpsopto::kGlElementArrayBuffer, 12);
  fpsopto_glBindBuffer(fpsopto::kGlElementArrayBuffer, 0);
  fpsopto_glBindBuffer(fpsopto::kGlElementArrayBuffer, 0);
  EXPECT_EQ(gTrace.count("glBindBuffer"), std::size_t{4});
}

void nonZeroBufferBindIsForwardedEvenWhenRepeated() {
  freshContext();
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 12);
  fpsopto_glBindBuffer(fpsopto::kGlArrayBuffer, 12);
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

// A non-zero framebuffer name is always forwarded, because the name can be
// recycled by a later glGenFramebuffers and the shadow cannot see that.
void repeatedFramebufferBindIsForwarded() {
  freshContext();
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 3);
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 3);
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{2});
}

// Binding the default framebuffer twice is the common end-of-frame call and is
// safe to drop, because 0 is a real name for the default framebuffer.
void repeatedDefaultFramebufferBindIsDropped() {
  freshContext();
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0);
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0);
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{1});
}

// A GL_READ_FRAMEBUFFER bind resets the draw-framebuffer shadow, because a
// blit can pair the read binding with a draw binding the shadow still holds.
void readFramebufferBindResetsDrawShadow() {
  freshContext();
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0);
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0); // redundant
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{1});

  // Binding a read framebuffer does not change the draw binding...
  fpsopto_glBindFramebuffer(fpsopto::kGlReadFramebuffer, 7);
  fpsopto_glBindFramebuffer(fpsopto::kGlReadFramebuffer, 7);
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{3});

  // ...but it invalidates the draw shadow, so a draw bind that would otherwise
  // look redundant must be issued again -- and only then is it redundant once
  // more.
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0); // forced by the reset
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{4});
  fpsopto_glBindFramebuffer(fpsopto::kGlFramebuffer, 0); // redundant again
  EXPECT_EQ(gTrace.count("glBindFramebuffer"), std::size_t{4});
}

void nonZeroVertexArrayBindIsForwarded() {
  freshContext();
  fpsopto_glBindVertexArray(5);
  fpsopto_glBindVertexArray(5);
  EXPECT_EQ(gTrace.count("glBindVertexArray"), std::size_t{2});
}

void repeatedVertexArrayUnbindIsDropped() {
  freshContext();
  fpsopto_glBindVertexArray(5);
  fpsopto_glBindVertexArray(0);
  fpsopto_glBindVertexArray(0);
  EXPECT_EQ(gTrace.count("glBindVertexArray"), std::size_t{2});
}

// ---------------------------------------------------------------------------
// Texture bindings, which are per unit
// ---------------------------------------------------------------------------

// A repeat of the same non-zero texture name is forwarded as well: the name may
// have been deleted and recycled since the shadow recorded it.
void repeatedTextureBindSameUnitIsForwarded() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  fpsopto_glBindTexture(fpsopto::kGlTexture2d, 10);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{2});
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

// A cube face target addresses the same slot as the cube map target, so a
// repeated non-zero bind is forwarded exactly like any other.
void cubeMapFacesAndTargetShareTheCubeSlot() {
  freshContext();
  fpsopto_glBindTexture(fpsopto::kGlTextureCubeMap, 20);
  fpsopto_glBindTexture(fpsopto::kGlTextureCubeMapPositiveX, 20);
  EXPECT_EQ(gTrace.count("glBindTexture"), std::size_t{2});
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

// A new context gets its own shadow. Setting state in one context must not
// make the same call redundant in a different context.
void contextsHaveIndependentShadows() {
  const std::uintptr_t first = makeContext();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend); // redundant here
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});

  makeContext();
  fpsopto_glEnable(fpsopto::kGlBlend); // real again in the new context
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});

  // Going back to the first context keeps its state: the capability is already
  // on there, so the call is redundant.
  fpsopto::GlInterceptor::instance().setCurrentContextForTest(first);
  gTrace.reset();
  fpsopto_glEnable(fpsopto::kGlBlend);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{0});
}

// A context this mod never saw created must be forwarded unfiltered: its real
// state is unknown, so no call against it can be assumed redundant.
void untrackedContextIsForwardedUnfiltered() {
  makeUntrackedContext();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glDisable(fpsopto::kGlBlend);
  fpsopto_glViewport(0, 0, 1, 1);
  fpsopto_glViewport(0, 0, 1, 1);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{2});
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{1});
  EXPECT_EQ(gTrace.count("glViewport"), std::size_t{2});
}

// Destroying a context drops its shadow, so a handle reused afterwards starts
// fresh rather than inheriting the destroyed context's state.
void destroyedContextShadowIsDropped() {
  const std::uintptr_t handle = makeContext();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend); // redundant
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});

  fpsopto::GlInterceptor::instance().noteContextDestroyed(handle);
  fpsopto::GlInterceptor::instance().noteContextCreated(handle);
  fpsopto::GlInterceptor::instance().setCurrentContextForTest(handle);
  gTrace.reset();
  fpsopto_glEnable(fpsopto::kGlBlend); // real again in the recreated context
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{1});
}

// Two rendering threads with different current contexts must not corrupt each
// other's shadow, which is what made the old single global copy unsafe when a
// loader thread made its context current. The loader leaves its own context
// with blending disabled; if the shadow were shared, the render thread's
// enable would be seen as a real change instead of a redundant one.
void threadShadowsAreIsolated() {
  const std::uintptr_t renderContext = makeContext();
  fpsopto_glEnable(fpsopto::kGlBlend); // render thread's context: blend on

  std::thread loader([]() {
    const std::uintptr_t loaderContext = 0x1000;
    fpsopto::GlInterceptor::instance().noteContextCreated(loaderContext);
    fpsopto::GlInterceptor::instance().setCurrentContextForTest(loaderContext);
    for (int i = 0; i < 1000; ++i) {
      fpsopto_glDisable(fpsopto::kGlBlend);
      fpsopto_glEnable(fpsopto::kGlBlend);
    }
    fpsopto_glDisable(fpsopto::kGlBlend); // ends with its own shadow "off"
  });
  loader.join();

  // The render thread's context is untouched: blend is still shadowed as on, so
  // re-enabling it is redundant and must not reach the driver.
  gTrace.reset();
  fpsopto_glEnable(fpsopto::kGlBlend);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{0});

  // ...and the loader's own context remembers its own state.
  fpsopto::GlInterceptor::instance().setCurrentContextForTest(0x1000);
  gTrace.reset();
  fpsopto_glDisable(fpsopto::kGlBlend); // already off there: redundant
  EXPECT_EQ(gTrace.count("glDisable"), std::size_t{0});

  fpsopto::GlInterceptor::instance().setCurrentContextForTest(renderContext);
  fpsopto::GlInterceptor::instance().noteContextDestroyed(0x1000);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void statisticsCountSuppressions() {
  freshContext();
  fpsopto::GlCounters::instance().reset();
  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend); // suppressed
  fpsopto_glViewport(0, 0, 1, 1);
  fpsopto::GlCounters::instance().publish();
  const auto after = gl().traffic();
  EXPECT_EQ(after.total.calls, std::uint64_t{3});
  EXPECT_EQ(after.total.suppressed, std::uint64_t{1});
  EXPECT_EQ(after.perHook[static_cast<std::size_t>(fpsopto::GlHook::Enable)].calls,
            std::uint64_t{2});
  EXPECT_EQ(
      after.perHook[static_cast<std::size_t>(fpsopto::GlHook::Enable)].suppressed,
      std::uint64_t{1});
  EXPECT_EQ(
      after.perHook[static_cast<std::size_t>(fpsopto::GlHook::Viewport)].calls,
      std::uint64_t{1});
  EXPECT_EQ(after.hooksDisabled, std::uint64_t{0});
}

// A hook that is switched off forwards its calls without dropping them, but it
// keeps learning them: the shadow it maintains is read by other hooks, so
// switching this hook back on must not find it stale.
void disabledHookForwardsEverything() {
  auto &interceptor = fpsopto::GlInterceptor::instance();
  freshContext();
  interceptor.setHookEnabled(fpsopto::GlHook::Enable, false);
  EXPECT_EQ(interceptor.traffic().hooksDisabled, std::uint64_t{1});

  fpsopto_glEnable(fpsopto::kGlBlend);
  fpsopto_glEnable(fpsopto::kGlBlend); // redundant, but the hook is off
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{2});

  interceptor.setHookEnabled(fpsopto::GlHook::Enable, true);
  EXPECT_EQ(interceptor.traffic().hooksDisabled, std::uint64_t{0});
  // The hook learned the state while it was off, so the repeated enable is
  // recognised at once and dropped.
  fpsopto_glEnable(fpsopto::kGlBlend);
  EXPECT_EQ(gTrace.count("glEnable"), std::size_t{2});
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
  repeatedProgramBindIsForwarded();
  repeatedProgramUnbindIsDropped();
  firstProgramBindIsForwarded();
  elementArrayBufferBindsAreAlwaysForwarded();
  nonZeroBufferBindIsForwardedEvenWhenRepeated();
  repeatedUnbindIsDropped();
  firstUnbindIsStillForwarded();
  repeatedFramebufferBindIsForwarded();
  repeatedDefaultFramebufferBindIsDropped();
  readFramebufferBindResetsDrawShadow();
  nonZeroVertexArrayBindIsForwarded();
  repeatedVertexArrayUnbindIsDropped();
  repeatedTextureBindSameUnitIsForwarded();
  sameTextureOnDifferentUnitsIsNotRedundant();
  differentTexturesOnSameUnitAreForwarded();
  cubeMapFacesAndTargetShareTheCubeSlot();
  unbindingTextureIsDroppedOnRepeat();
  bindsOnUntrackedUnitsAreForwarded();
  repeatedActiveTextureIsDropped();
  repeatedPixelStoreIsDropped();
  unknownPixelStorePnameIsForwarded();
  contextResetRestoresSpecDefaults();
  contextsHaveIndependentShadows();
  untrackedContextIsForwardedUnfiltered();
  destroyedContextShadowIsDropped();
  threadShadowsAreIsolated();
  statisticsCountSuppressions();
  disabledHookForwardsEverything();
}