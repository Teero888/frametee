// Small vector, quaternion and frustum helpers shared by the TMNF module.
//
// Nothing here knows about ForeverValidator or the engine beyond ft_vec3, so it
// stays usable from every translation unit without dragging the simulation
// headers along.

#ifndef TMNF_MATH_H
#define TMNF_MATH_H

#include <frametee/game_abi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tmnf {

inline constexpr float kPi = 3.14159265358979323846f;

// --- vectors -----------------------------------------------------------------

inline ft_vec3 Add(ft_vec3 a, ft_vec3 b) { return ft_vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline ft_vec3 Sub(ft_vec3 a, ft_vec3 b) { return ft_vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline ft_vec3 Scale(ft_vec3 v, float s) { return ft_vec3{v.x * s, v.y * s, v.z * s}; }
inline ft_vec3 Mul(ft_vec3 a, ft_vec3 b) { return ft_vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(ft_vec3 a, ft_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float LengthSq(ft_vec3 v) { return Dot(v, v); }
inline float Length(ft_vec3 v) { return std::sqrt(Dot(v, v)); }

inline ft_vec3 Cross(ft_vec3 a, ft_vec3 b) {
  return ft_vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline ft_vec3 Normalize(ft_vec3 v, ft_vec3 fallback = ft_vec3{0.f, 1.f, 0.f}) {
  const float len = Length(v);
  return len > 1e-6f ? Scale(v, 1.f / len) : fallback;
}

inline ft_vec3 Lerp(ft_vec3 a, ft_vec3 b, float t) { return Add(a, Scale(Sub(b, a), t)); }

// Drops the vertical component. Camera work wants a heading rather than the
// car's own forward, because the engine's view matrix is built with a fixed
// world up and any roll baked into a direction would simply be discarded.
inline ft_vec3 FlattenY(ft_vec3 v, ft_vec3 fallback = ft_vec3{0.f, 0.f, 1.f}) {
  const ft_vec3 flat{v.x, 0.f, v.z};
  return LengthSq(flat) > 1e-8f ? Normalize(flat) : fallback;
}

// --- colours -----------------------------------------------------------------

// Triangles are stored by the hundred thousand, so their colour is packed into
// one word instead of four floats.
inline std::uint32_t PackColor(ft_color c) {
  const auto q = [](float v) {
    return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
  };
  return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
}

inline ft_color UnpackColor(std::uint32_t v) {
  constexpr float kInv = 1.f / 255.f;
  return ft_color{static_cast<float>(v & 0xFFu) * kInv, static_cast<float>((v >> 8) & 0xFFu) * kInv,
                  static_cast<float>((v >> 16) & 0xFFu) * kInv, static_cast<float>((v >> 24) & 0xFFu) * kInv};
}

inline ft_color MixColor(ft_color a, ft_color b, float t) {
  return ft_color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// --- quaternions -------------------------------------------------------------

struct Quat {
  float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

inline Quat Conjugate(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

inline Quat NormalizeQuat(Quat q) {
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 1e-8f) return Quat{};
  return Quat{q.x / len, q.y / len, q.z / len, q.w / len};
}

inline Quat Slerp(Quat q0, Quat q1, float t) {
  float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
  if (dot < 0.f) {
    q1 = Quat{-q1.x, -q1.y, -q1.z, -q1.w};
    dot = -dot;
  }
  if (dot > 0.9995f) {
    return NormalizeQuat(Quat{q0.x + (q1.x - q0.x) * t, q0.y + (q1.y - q0.y) * t, q0.z + (q1.z - q0.z) * t,
                              q0.w + (q1.w - q0.w) * t});
  }
  const float theta_0 = std::acos(std::clamp(dot, -1.f, 1.f));
  const float theta = theta_0 * t;
  const float sin_theta = std::sin(theta);
  const float sin_theta_0 = std::sin(theta_0);
  const float s0 = std::cos(theta) - dot * sin_theta / sin_theta_0;
  const float s1 = sin_theta / sin_theta_0;
  return Quat{s0 * q0.x + s1 * q1.x, s0 * q0.y + s1 * q1.y, s0 * q0.z + s1 * q1.z, s0 * q0.w + s1 * q1.w};
}

// The rotation `a` followed by `b`, in the sense that Rotate(Concat(b, a), v)
// applies `a` first. A steered, spinning wheel needs exactly this: it turns
// about its own axle and the whole assembly is then turned by the steering.
inline Quat Concat(Quat b, Quat a) {
  return Quat{b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y, b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
              b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w, b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z};
}

// A rotation of `angle` radians about a unit axis.
inline Quat QuatFromAxisAngle(ft_vec3 axis, float angle) {
  const float half = angle * 0.5f;
  const float s = std::sin(half);
  return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

inline ft_vec3 Rotate(Quat q, ft_vec3 v) {
  const ft_vec3 u{q.x, q.y, q.z};
  const float s = q.w;
  return Add(Add(Scale(u, 2.f * Dot(u, v)), Scale(v, s * s - Dot(u, u))), Scale(Cross(u, v), 2.f * s));
}

// The car's own axes. TrackMania is left handed with +Y up and the car looking
// down +Z, which is what the basis below reproduces.
inline void AxesFromQuat(Quat q, ft_vec3 *forward, ft_vec3 *right, ft_vec3 *up) {
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  if (right) *right = ft_vec3{1.f - 2.f * (y * y + z * z), 2.f * (x * y + w * z), 2.f * (x * z - w * y)};
  if (up) *up = ft_vec3{2.f * (x * y - w * z), 1.f - 2.f * (x * x + z * z), 2.f * (y * z + w * x)};
  if (forward) *forward = ft_vec3{2.f * (x * z + w * y), 2.f * (y * z - w * x), 1.f - 2.f * (x * x + y * y)};
}

// --- axis aligned boxes ------------------------------------------------------

struct Aabb {
  ft_vec3 mn{1e30f, 1e30f, 1e30f};
  ft_vec3 mx{-1e30f, -1e30f, -1e30f};

  bool Valid() const { return mn.x <= mx.x; }
  void Add(ft_vec3 p) {
    mn.x = std::min(mn.x, p.x);
    mn.y = std::min(mn.y, p.y);
    mn.z = std::min(mn.z, p.z);
    mx.x = std::max(mx.x, p.x);
    mx.y = std::max(mx.y, p.y);
    mx.z = std::max(mx.z, p.z);
  }
  void Merge(const Aabb &o) {
    if (!o.Valid()) return;
    Add(o.mn);
    Add(o.mx);
  }
  ft_vec3 Center() const { return Scale(::tmnf::Add(mn, mx), 0.5f); }
  ft_vec3 Extent() const { return Scale(Sub(mx, mn), 0.5f); }
};

// --- frustum -----------------------------------------------------------------

// Six clip planes pulled straight out of the engine's view-projection, so the
// module culls against what is actually on screen rather than a guessed radius
// around the car. The matrix is column major and the clip volume is Vulkan's
// (depth in [0, w]), matching what the renderer builds.
struct Frustum {
  // Plane k is {x, y, z, d} with "inside" meaning n.p + d >= 0.
  float plane[6][4] = {};

  static Frustum FromViewProj(const float m[16]) {
    const auto row = [m](int k, int i) { return m[k + 4 * i]; };
    Frustum f;
    const auto set = [&](int index, float a, float b, float c, float d) {
      const float len = std::sqrt(a * a + b * b + c * c);
      const float inv = len > 1e-8f ? 1.f / len : 0.f;
      f.plane[index][0] = a * inv;
      f.plane[index][1] = b * inv;
      f.plane[index][2] = c * inv;
      f.plane[index][3] = d * inv;
    };
    // Rows in the sense clip.k = row(k) . (x, y, z, 1).
    const float r0[4] = {row(0, 0), row(0, 1), row(0, 2), row(0, 3)};
    const float r1[4] = {row(1, 0), row(1, 1), row(1, 2), row(1, 3)};
    const float r2[4] = {row(2, 0), row(2, 1), row(2, 2), row(2, 3)};
    const float r3[4] = {row(3, 0), row(3, 1), row(3, 2), row(3, 3)};
    set(0, r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]); // left
    set(1, r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]); // right
    set(2, r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]); // bottom
    set(3, r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]); // top
    // Zero-to-one clip space, so the two depth planes are z >= 0 and w - z >= 0
    // rather than the symmetric pair the OpenGL convention uses. The engine
    // renders with a reversed range, which swaps which of these is the near
    // plane and which is the far one — and leaves the volume they bound
    // identical, which is why nothing here has to know about it.
    set(4, r2[0], r2[1], r2[2], r2[3]);                                 // near
    set(5, r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]); // far
    return f;
  }

  bool Valid() const {
    // A zeroed camera (a 2D game, or the very first frame) must not cull
    // everything away, so an unusable matrix is reported as "no frustum".
    for (int k = 0; k < 6; ++k) {
      if (plane[k][0] != 0.f || plane[k][1] != 0.f || plane[k][2] != 0.f) return true;
    }
    return false;
  }

  bool Intersects(const Aabb &box) const {
    if (!box.Valid()) return false;
    const ft_vec3 c = box.Center();
    const ft_vec3 e = box.Extent();
    for (int k = 0; k < 6; ++k) {
      const float nx = plane[k][0], ny = plane[k][1], nz = plane[k][2];
      const float radius = e.x * std::fabs(nx) + e.y * std::fabs(ny) + e.z * std::fabs(nz);
      const float distance = nx * c.x + ny * c.y + nz * c.z + plane[k][3];
      if (distance + radius < 0.f) return false;
    }
    return true;
  }
};

} // namespace tmnf

#endif // TMNF_MATH_H
