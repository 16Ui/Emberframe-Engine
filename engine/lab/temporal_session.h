#pragma once
#include "postprocess.h"
#include <atomic>
#include <mutex>

namespace emberframe::lab {

struct ReferenceSessionInfo {
    // Successful frames in the current scene/settings sequence. The next render
    // uses this as its zero-based Halton/seed index. Cancelled work consumes none.
    std::uint64_t frames = 0;
    std::uint64_t reset_count = 0;
    std::uint32_t frame_seed = 0;
    glm::vec2 jitter_ndc{0};
    // Fraction of valid current surfaces with accepted temporal history, and the
    // maximum per-pixel history age. Sky has no valid surface and is excluded.
    float reused_fraction = 0;
    float max_history_length = 0;
    // Sequence identity restarted; per-pixel rejection / detected camera cuts
    // inside TemporalFilter are reflected by coverage and history age instead.
    bool history_reset = false;
};

// Persistent CPU reference pipeline. Keep one instance per viewport and call
// render() on every background frame; recreating it per job disables accumulation.
//
// Stages: true jittered render_reference -> TemporalFilter -> optional NPR ->
// additive bloom ONCE -> requested debug. All returned color is linear HDR;
// exposure/tone mapping/sRGB remain the display/upload owner's responsibility.
// Returned variance is raw TemporalFilter output variance. DebugView::variance
// displays grayscale sqrt(variance)/(1+sqrt(variance)); zero when filters are off.
// indirect/ao/shadow/surfaces retain the current reference frame's diagnostics.
//
// Scene revision AND rendering-content fingerprints invalidate history. Pointers
// are never used as identity: copied background-job snapshots keep history, while
// edits to geometry/material/light/texture/transform clear it even if revision was
// not incremented. The fingerprint scans input data (O(scene size)); no asset I/O.
// Node::previous_world is correspondence metadata and is excluded from identity.
// As transform edits start a new sequence, the session normalizes stale previous
// transforms to the current resolved world transforms before reference rendering.
// Sampling/lighting/filter-mode/size/base-seed changes invalidate; debug/exposure/
// bloom/outline/hatching do not. Camera movement is NOT an invalidation key:
// previous-camera reprojection and per-surface checks decide history validity.
// TemporalFilter still detects large cuts; call reset() for an explicit small cut.
//
// Motion follows reference_renderer: unjittered currentPixel-previousPixel, pixel
// Y down. TAA's Halton offset is passed into actual projection/rays and reported
// to TemporalFilter; merely changing a seed is not projection jitter.
// Finite samples and screen-space history cannot recover hidden/missing geometry.
// Denoising currently reconstructs full HDR, without albedo demodulation.
//
// render/reset are serialized internally; info() reads the last committed stats
// without waiting for an in-flight render. Do not destroy the session while a
// background render is using it. Inputs must remain immutable during the call.
// Cancel flag must remain set until render returns. Cancellation returns an EMPTY
// RenderOutput and leaves committed history/frame sequence unchanged; discard it.
// Renderer/filter exceptions propagate, also without committing partial history.
class ReferenceSession {
public:
    ReferenceSession() = default;
    ReferenceSession(const ReferenceSession&) = delete;
    ReferenceSession& operator=(const ReferenceSession&) = delete;

    RenderOutput render(const Scene&, const Camera&, const Settings&,
                        std::atomic<bool>* cancel = nullptr);
    void reset();
    ReferenceSessionInfo info() const;

private:
    mutable std::mutex mutex_;
    mutable std::mutex info_mutex_;
    TemporalFilter temporal_;
    Camera previous_camera_;
    std::uint64_t scene_key_ = 0, settings_key_ = 0;
    ReferenceSessionInfo info_;
    bool ready_ = false;
};

TestResults test_temporal_session();

} // namespace emberframe::lab
