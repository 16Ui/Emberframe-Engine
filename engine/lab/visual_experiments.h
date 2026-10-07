#pragma once
#include "types.h"
#include <atomic>

namespace emberframe::lab {
// Supported zero-based topic IDs: 10(D11), 11(D12), 12(D13), 29(D30).
bool supports_visual_experiment(int topic0based);
// Shared, user-facing Chinese guidance for the main app's topic buttons. Describes
// panel order, controls, camera behavior and approximations; unsupported IDs throw.
std::string visual_experiment_guide(int topic0based);

// Linear HDR output; main app owns exposure/tone mapping. Image dimensions are the
// requested Settings dimensions (positive, <=4M pixels). Cancel matches the existing
// render_reference API; true returns a cleared/partially filled, dimensioned frame.
// No files are saved and no Scene/Camera/Settings passed by the caller are mutated.
//
// D11: two copies of the current scene/camera. Left = cosine-sampled environment
// diffuse reference, right = SH9 cosine convolution; neither includes occlusion.
// D12: left = unoccluded SH9; right = per-vertex diffuse PRT with BVH visibility,
// barycentrically interpolated after baking ONCE, then lit by the same environment.
// Empty scene falls back to a built-in floor and two boxes at (0,.5,0).
// D13: 3x3 atlas: source environment, diffuse irradiance/pi, BRDF A/B lookup;
// bottom six tiles are GGX-prefiltered roughness levels 0,.2,.4,.6,.8,1.
// Two documented analytic HDR lobes augment the scene sky in D13 to reveal filtering.
// D30: two copies of a built-in sphere/box/floor signed-distance scene. Left analytic,
// right trilinear baked SDF; BOTH sphere trace surfaces and cast SDF soft shadows.
// D30 auto-aims at (0,.5,0), preserves orbit direction, and bounds camera distance.
// Only the first scene light is used for D30; an empty list gets a directional key.
// samples controls bounded integration quality; voxel_resolution controls SDF bake.
// Debug views normal/depth/shadow/indirect/albedo remain available for geometry cases.
// PRT is static diffuse direct transfer, not glossy or multibounce PRT; SDF shadows
// are a penumbra heuristic, not exact area-light integration. D13 has no G-buffer.
// SH/PRT use interpolated mesh normals (no normal-map transfer). The D13 atlas uses
// bounded 256-1024 integration samples so the diagnostic HDR spots remain readable.
RenderOutput run_visual_experiment(int topic0based,const Scene&,Camera,Settings,
                                   std::atomic<bool>* cancel=nullptr);
TestResults test_visual_experiments();
}
