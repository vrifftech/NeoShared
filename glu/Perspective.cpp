#include <neoshared/glu/Perspective.hpp>

#include <cmath>

namespace neoshared::glu {
namespace {
// Retain the SSE2 binary64 operation boundaries on hosts using wider registers.
double multiply(double a,double b) { volatile double value=a*b;return value; }
double divide(double a,double b) { volatile double value=a/b;return value; }
double add(double a,double b) { volatile double value=a+b;return value; }
double subtract(double a,double b) { volatile double value=a-b;return value; }
}

bool perspective(double fieldOfViewDegrees, double aspect,
                 double nearPlane, double farPlane,
                 std::array<float, 16>& matrix) {
    // Android 0x00905c58..0x00905c7b: retain the separate double multiplies
    // and division, then evaluate sine before the three native zero checks.
    const double radians = divide(multiply(multiply(fieldOfViewDegrees,0.5),
                                           3.14159265358979323846),180.0);
    const double sine = std::sin(radians);
    if (aspect == 0.0) return false;
    const double depthDifference = subtract(farPlane,nearPlane);
    if (depthDifference == 0.0) return false;
    if (sine == 0.0) return false;

    const double cotangent = divide(std::cos(radians),sine);
    // 0x00905d14..0x00905d90: each populated entry is narrowed to float,
    // then the column-major matrix is passed to ASLgl::glMultMatrixf.
    matrix = {
        static_cast<float>(divide(cotangent,aspect)), 0.0f, 0.0f, 0.0f,
        0.0f, static_cast<float>(cotangent), 0.0f, 0.0f,
        0.0f, 0.0f,
        static_cast<float>(divide(-add(nearPlane,farPlane),depthDifference)), -1.0f,
        0.0f, 0.0f,
        static_cast<float>(divide(multiply(multiply(nearPlane,-2.0),farPlane),depthDifference)),
        0.0f,
    };
    return true;
}

} // namespace neoshared::glu
