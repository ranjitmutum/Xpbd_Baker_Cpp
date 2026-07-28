#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace xpbd::render {


struct ViewportCamera {
    float yaw_deg = 35.0f;
    float pitch_deg = 25.0f;
    float distance = 50.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    float pan_z = 0.0f;
    float fov_y_deg = 45.0f;
    float near_z = 0.1f;

    void reset() {
        yaw_deg = 35.0f;
        pitch_deg = 25.0f;
        distance = 50.0f;
        pan_x = pan_y = pan_z = 0.0f;
    }

    void orbit(float dx_px, float dy_px, float sensitivity = 0.35f) {
        yaw_deg -= dx_px * sensitivity;
        pitch_deg = std::clamp(pitch_deg + dy_px * sensitivity, -89.0f, 89.0f);
    }






    void pan(float dx_px, float dy_px, float sensitivity = 0.02f) {
        const float yaw = yaw_deg * 0.017453292519943295f;
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);


        const float right_x = cy;
        const float right_z = -sy;
        const float fwd_x = -sy;
        const float fwd_z = -cy;
        const float scale = std::max(0.5f, distance) * sensitivity * 0.14f;
        pan_x += (right_x * dx_px + fwd_x * dy_px) * scale;
        pan_z += (right_z * dx_px + fwd_z * dy_px) * scale;
    }


    void panHeight(float delta_px, float sensitivity = 0.02f) {
        const float scale = std::max(0.5f, distance) * sensitivity * 0.16f;
        pan_y += delta_px * scale;
    }

    void zoom(float scroll_y, float sensitivity = 0.08f) {
        distance = std::clamp(distance * (1.0f - static_cast<float>(scroll_y) * sensitivity),
                              2.0f, 500.0f);
    }


    void fit(const std::array<float, 3>& center, float radius) {
        pan_x = center[0];
        pan_y = center[1];
        pan_z = center[2];
        distance = std::clamp(radius * 2.8f + 4.0f, 8.0f, 400.0f);
        pitch_deg = 25.0f;
        yaw_deg = 35.0f;
    }


    void eye(float& out_x, float& out_y, float& out_z) const {
        const float yaw = yaw_deg * 0.017453292519943295f;
        const float pitch = pitch_deg * 0.017453292519943295f;
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float cp = std::cos(pitch);
        const float sp = std::sin(pitch);
        out_x = pan_x + distance * cp * sy;
        out_y = pan_y + distance * sp;
        out_z = pan_z + distance * cp * cy;
    }


    void viewAxes(float& rx, float& ry, float& rz, float& ux, float& uy, float& uz) const {
        float eye_x = 0, eye_y = 0, eye_z = 0;
        eye(eye_x, eye_y, eye_z);
        float fx = pan_x - eye_x;
        float fy = pan_y - eye_y;
        float fz = pan_z - eye_z;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-6f) {
            fx = 0;
            fy = 0;
            fz = -1;
        } else {
            fx /= fl;
            fy /= fl;
            fz /= fl;
        }

        rx = -fz;
        ry = 0.0f;
        rz = fx;
        float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-6f) {
            rx = 1.0f;
            ry = 0.0f;
            rz = 0.0f;
            rl = 1.0f;
        }
        rx /= rl;
        ry /= rl;
        rz /= rl;
        ux = ry * fz - rz * fy;
        uy = rz * fx - rx * fz;
        uz = rx * fy - ry * fx;
        const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (ul > 1e-6f) {
            ux /= ul;
            uy /= ul;
            uz /= ul;
        }
    }





    void matrices(float aspect, float* out_view16, float* out_proj16) const {
        float eye_x = 0, eye_y = 0, eye_z = 0;
        eye(eye_x, eye_y, eye_z);

        float fx = pan_x - eye_x;
        float fy = pan_y - eye_y;
        float fz = pan_z - eye_z;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-6f) {
            fx = 0;
            fy = 0;
            fz = -1;
        } else {
            fx /= fl;
            fy /= fl;
            fz /= fl;
        }

        float rx = -fz;
        float ry = 0.0f;
        float rz = fx;
        float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-6f) {
            rx = 1.0f;
            ry = 0.0f;
            rz = 0.0f;
            rl = 1.0f;
        }
        rx /= rl;
        ry /= rl;
        rz /= rl;
        const float ux = ry * fz - rz * fy;
        const float uy = rz * fx - rx * fz;
        const float uz = rx * fy - ry * fx;



        out_view16[0] = rx;
        out_view16[1] = ux;
        out_view16[2] = -fx;
        out_view16[3] = 0.0f;
        out_view16[4] = ry;
        out_view16[5] = uy;
        out_view16[6] = -fy;
        out_view16[7] = 0.0f;
        out_view16[8] = rz;
        out_view16[9] = uz;
        out_view16[10] = -fz;
        out_view16[11] = 0.0f;
        out_view16[12] = -(rx * eye_x + ry * eye_y + rz * eye_z);
        out_view16[13] = -(ux * eye_x + uy * eye_y + uz * eye_z);
        out_view16[14] = -(-fx * eye_x - fy * eye_y - fz * eye_z);
        out_view16[15] = 1.0f;

        const float a = (aspect > 1e-6f) ? aspect : 1.0f;
        const float f = 1.0f / std::tan(fov_y_deg * 0.017453292519943295f * 0.5f);
        const float near_p = near_z;
        const float far_p = (std::max)(distance * 20.0f + 50.0f, 200.0f);

        for (int i = 0; i < 16; ++i) {
            out_proj16[i] = 0.0f;
        }
        out_proj16[0] = f / a;
        out_proj16[5] = f;
        out_proj16[10] = (far_p + near_p) / (near_p - far_p);
        out_proj16[11] = -1.0f;
        out_proj16[14] = (2.0f * far_p * near_p) / (near_p - far_p);
    }





    [[nodiscard]] bool project(float wx, float wy, float wz, float view_w, float view_h,
                               float& out_x, float& out_y, float& out_depth) const {
        const float yaw = yaw_deg * 0.017453292519943295f;
        const float pitch = pitch_deg * 0.017453292519943295f;
        const float cy = std::cos(yaw);
        const float sy = std::sin(yaw);
        const float cp = std::cos(pitch);
        const float sp = std::sin(pitch);


        const float eye_x = pan_x + distance * cp * sy;
        const float eye_y = pan_y + distance * sp;
        const float eye_z = pan_z + distance * cp * cy;


        float fx = pan_x - eye_x;
        float fy = pan_y - eye_y;
        float fz = pan_z - eye_z;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-6f) {
            return false;
        }
        fx /= fl;
        fy /= fl;
        fz /= fl;



        float rx = -fz;
        float ry = 0.0f;
        float rz = fx;
        float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (rl < 1e-6f) {

            rx = 1.0f;
            ry = 0.0f;
            rz = 0.0f;
            rl = 1.0f;
        }
        rx /= rl;
        ry /= rl;
        rz /= rl;
        const float ux = ry * fz - rz * fy;
        const float uy = rz * fx - rx * fz;
        const float uz = rx * fy - ry * fx;

        const float dx = wx - eye_x;
        const float dy = wy - eye_y;
        const float dz = wz - eye_z;
        const float cam_x = dx * rx + dy * ry + dz * rz;
        const float cam_y = dx * ux + dy * uy + dz * uz;
        const float cam_z = dx * fx + dy * fy + dz * fz;
        if (cam_z <= near_z) {
            return false;
        }

        const float aspect = (view_h > 1.0f) ? (view_w / view_h) : 1.0f;
        const float f = 1.0f / std::tan(fov_y_deg * 0.017453292519943295f * 0.5f);
        const float ndc_x = (cam_x * f) / (aspect * cam_z);
        const float ndc_y = (cam_y * f) / cam_z;
        out_x = (ndc_x * 0.5f + 0.5f) * view_w;

        out_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * view_h;
        out_depth = cam_z;
        return true;
    }
};

}
