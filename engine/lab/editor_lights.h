#pragma once
#include "types.h"
#include <cstddef>

namespace emberframe::lab {
// Checks a proposed edit without changing the scene. A binding is supported only
// for a rectangle with a unique, planar XZ emitter (local normals -Y). Parents
// may include rotation, scale or shear, but must be finite, affine and invertible.
// Unbound lights, including imported punctual lights, remain independent of nodes.
void validate_light_binding(const Scene&, std::size_t light, const Light& replacement);
void validate_light_binding(const Scene&, std::size_t light);

// Applies world-space center/direction/full width and height, and color*intensity
// emission. Mesh bounds account for non-unit/off-center emitter geometry. Shared
// mesh/material resources are isolated as needed, including LOD material slots.
// Errors leave all scene values unchanged. Successful edits increase revision;
// previous_world remains frame history. Call on edits, not on every frame.
// Use UndoStack::begin/commit/cancel around a drag to retain the complete change.
void update_editor_light(Scene&, std::size_t light, const Light& replacement);
void set_editor_light_position(Scene&, std::size_t light, glm::vec3 position);

// Returns the new light index. Rectangle creates a dedicated unit XZ emitter and
// emissive material; directional and point lights create no geometry. Atomic on
// failure. The new node's previous_world is initialized to its world transform.
// Creation rejects scenes already at the renderer's 64-light limit.
int add_editor_light(Scene&, LightKind);

TestResults test_editor_lights();
} // namespace emberframe::lab
