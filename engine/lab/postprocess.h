#pragma once
#include "types.h"

namespace emberframe::lab {

// CPU reference, linear HDR throughout. Pixels have a top-left origin (+Y down).
// G-buffer positions/normals are world-space; linear_depth = -view_position.z > 0.
// Malformed image extents throw; invalid geometry is background. Nonfinite HDR
// channels become zero and finite channels are bounded to [0,1e10] for safe moments.

// Returns visibility (1 = unoccluded). SSAO uses projected cosine-hemisphere
// samples; GTAO analytically integrates cosine-weighted horizon slices. Both see
// only the nearest screen surface: offscreen/hidden/thin geometry is unresolved.
// ao_radius is world units. samples controls bounded CPU quality; GTAO searches
// at most 96 pixels. No multibounce correction or temporal AO filtering is implied.
Image<float> ambient_occlusion(const GBuffer&, const Camera&, const Settings&);

// Returns ONLY strength * multilevel bloom, for caller-side addition to HDR.
// Disabled bloom returns a zero image of the same extent. Threshold is linear peak RGB;
// extraction preserves hue. Six normalized Gaussian pyramid levels are averaged,
// not summed without normalization. DC is preserved by the blur; finite-image
// boundaries/odd-sized resampling need not preserve an impulse's exact total flux.
Image<glm::vec3> bloom(const Image<glm::vec3>& hdr, const Settings&);

// HW5 spatial stage: normal/position/depth/albedo/object-guided joint bilateral.
// No radiance range term (so Monte Carlo noise itself cannot block smoothing).
Image<glm::vec3> joint_bilateral_filter(const Image<glm::vec3>&, const GBuffer&,
                                     int radius = 2);

enum class MotionConvention {
    // Main-renderer convention: motion = currentPixel - previousPixel, in pixels
    // (+Y down), excluding jitter. A zero vector falls back to previous-camera
    // projection of the current world position (static geometry). For moving
    // geometry whose camera/object motions cancel to exactly zero, use explicit.
    current_minus_previous_pixels,
    // Same sign/units, but zero is a valid total zero displacement, never fallback.
    current_minus_previous_pixels_explicit,
    // Optional alternative: previous-camera projection of CURRENT world position + motion.
    // motion is the object's backward pixel displacement in the PREVIOUS view:
    // project(previous object position) - project(current object position).
    // Static geometry must use zero; camera motion is already in reprojection.
    world_plus_object_pixels,
    // Full backward displacement: previous unjittered pixel - current unjittered
    // pixel. Includes camera AND object motion. Zero means no screen displacement.
    backward_pixels,
    // Same full displacement, in NDC (+Y up): pixel_delta = motion*(W/2,-H/2).
    backward_ndc
};

struct TemporalInput {
    MotionConvention motion = MotionConvention::current_minus_previous_pixels;
    // Actual jitter used to rasterize this frame, not merely the next suggested
    // sample. Add to clip.xy as jitter_ndc*clip.w. Default zero supports callers
    // that have not integrated projection jitter. Motion excludes jitter.
    glm::vec2 jitter_ndc{0};
    bool camera_cut = false;
    // Optional previous-frame WORLD position of each current surface, indexed by
    // CURRENT pixel. Supply for moving geometry to validate previous-view depth.
    // Without it, lateral motion works; motion in depth is conservatively rejected
    // when its depth change exceeds tolerance. World normals must also be stable:
    // strongly rotating surfaces deliberately restart history.
    const Image<glm::vec3>* previous_world_positions = nullptr;
};

class TemporalFilter {
public:
    void reset();
    Image<glm::vec3> process(const Image<glm::vec3>& current, const GBuffer&,
                             const Camera&, const Settings&);
    Image<glm::vec3> process(const Image<glm::vec3>& current, const GBuffer&,
                             const Camera&, const Settings&, const TemporalInput&);
    const Image<float>& variance() const { return variance_; }
    const Image<float>& history_validity() const { return valid_; }
    const Image<float>& history_length() const { return length_; }
    const Image<glm::vec2>& moments() const { return moments_; }

    // taa: reprojection, per-tap depth/normal/ID tests, YCoCg neighborhood clamp,
    // EMA. denoise: HW5 bilateral BEFORE that temporal stage. svgf takes priority
    // over denoise: raw-signal moments, short-history spatial variance bootstrap,
    // then four 5x5 B-spline a-trous passes guided by geometry/variance/luminance.
    // temporal_weight is CURRENT-frame alpha, clamped to [0,1]. SVGF additionally
    // uses 1/history_length during warmup. variance() is propagated output variance;
    // moments() retains pre-a-trous temporal luminance moments. Temporal (not final
    // spatial) color is fed back to avoid repeated spatial overblur. No albedo
    // demodulation, specular lobe tracking, or paper-exact GPU optimization.
    // Resize, mode/convention switch and detected large camera cuts reset history.
    // Use reset()/camera_cut for scene revisions, exposure discontinuities and
    // smaller editorial cuts; those cannot reliably be inferred from geometry.
private:
    Image<glm::vec3> history_;
    GBuffer previous_;
    Image<glm::vec2> moments_;
    Image<float> variance_, valid_, length_;
    Camera previous_camera_;
    glm::vec2 previous_jitter_{0};
    MotionConvention previous_motion_ = MotionConvention::current_minus_previous_pixels;
    int previous_mode_ = -1;
    bool ready_ = false;
};

// Halton(2,3), zero-based frame. NDC offset, <= half a pixel each axis.
// Does not modify a camera or advance TemporalFilter; use TemporalInput to report
// the actual raster jitter. Nonpositive dimensions return zero.
glm::vec2 taa_jitter(std::uint64_t frame, int width, int height);

// Optional four-band luminance toon mapping, one-pixel depth/normal/ID/silhouette
// outline, and luminance-dependent crosshatching. Hatching is in screen pixels
// (not object-attached); camera motion can therefore cause pattern swimming.
// This is screen-space NPR, not an inverted-hull geometry outline.
Image<glm::vec3> apply_npr(const Image<glm::vec3>&, const GBuffer&, const Settings&);

TestResults test_postprocess();

} // namespace emberframe::lab
