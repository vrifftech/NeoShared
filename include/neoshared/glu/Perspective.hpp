#pragma once

#include <array>

namespace neoshared::glu {

// Column-major matrix from Android's embedded gluPerspective (0x00905c30).
// All arithmetic precedes the native float narrowing; trigonometry uses the
// host's double-precision math library. No Apple GLU equivalence is implied.
// False is the native no-op for zero aspect, depth difference or sine; matrix
// is left unchanged. Camera policy and multiplication belong to the caller.
bool perspective(double fieldOfViewDegrees, double aspect,
                 double nearPlane, double farPlane,
                 std::array<float, 16>& matrix);

} // namespace neoshared::glu
