#pragma once

#include <cmath>
#include <string>

namespace xpbd::models {


struct Vector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vector3() = default;
    Vector3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vector3(const Vector3& other) = default;
    Vector3& operator=(const Vector3& other) = default;

    Vector3& set(double x_, double y_, double z_) {
        x = x_;
        y = y_;
        z = z_;
        return *this;
    }

    Vector3& set(const Vector3& v) { return set(v.x, v.y, v.z); }

    Vector3& sub(const Vector3& v) {
        x -= v.x;
        y -= v.y;
        z -= v.z;
        return *this;
    }

    [[nodiscard]] double length() const { return std::sqrt(x * x + y * y + z * z); }

    [[nodiscard]] Vector3 copy() const { return Vector3(*this); }
};

}
