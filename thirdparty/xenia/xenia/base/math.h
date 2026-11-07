// Minimal stand-in for Xenia's base headers, enough for gpu/ucode.h and gpu/xenos.h.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <type_traits>
#ifndef XE_NOALIAS
#define XE_NOALIAS
#endif
#define XE_FORCEINLINE inline
#define XE_NOINLINE
#define XE_RESTRICT __restrict__
#define assert_true(x) ((void)0)
#define assert_false(x) ((void)0)
#define assert_zero(x) ((void)0)
#define assert_not_zero(x) ((void)0)
#define assert_always(...) ((void)0)
#define assert_unhandled_case(x) ((void)0)
#define assert_not_null(x) ((void)0)
#define static_assert_size(type, size) static_assert(sizeof(type) == size, "bad size")
#include <cassert>
namespace xe {
using fourcc_t = uint32_t;
constexpr fourcc_t make_fourcc(const char (&s)[5]) { return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3])); }
constexpr fourcc_t make_fourcc(char a, char b, char c, char d) { return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d)); }
template <typename T> using le = T;
template <typename T> using be = T;
template <typename T, size_t N> constexpr size_t countof(T (&)[N]) { return N; }
template <typename T> constexpr T align(T v, T a) { return (v + a - 1) / a * a; }
template <typename T> constexpr T round_up(T v, T a, bool = true) { return (v + a - 1) / a * a; }
inline uint32_t log2_ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
inline uint32_t log2_floor(uint32_t v) { uint32_t r = 0; while (v >>= 1) r++; return r; }
template <typename T> inline T byte_swap(T v) {
  if constexpr (sizeof(T) == 2) return T(__builtin_bswap16(uint16_t(v)));
  else if constexpr (sizeof(T) == 4) return T(__builtin_bswap32(uint32_t(v)));
  else if constexpr (sizeof(T) == 8) return T(__builtin_bswap64(uint64_t(v)));
  else return v;
}
inline bool bit_scan_forward(uint32_t v, uint32_t* out) { if (!v) return false; *out = __builtin_ctz(v); return true; }
inline bool bit_scan_forward(uint64_t v, uint32_t* out) { if (!v) return false; *out = __builtin_ctzll(v); return true; }
inline uint32_t bit_count(uint32_t v) { return __builtin_popcount(v); }
template <typename T> inline T clamp_float(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }
inline float saturate(float v) { return std::min(std::max(v, 0.0f), 1.0f); }
}
namespace xe {
template <typename T> inline void copy_and_swap(T* dst, const T* src, size_t count) {
  for (size_t i = 0; i < count; i++) dst[i] = byte_swap(src[i]);
}
}
namespace xe {
inline uint32_t lzcnt(uint32_t v) { return v ? __builtin_clz(v) : 32; }
inline uint32_t lzcnt(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
namespace memory {
template <typename T, typename F> inline T Reinterpret(F f) { static_assert(sizeof(T) == sizeof(F)); T t; std::memcpy(&t, &f, sizeof(T)); return t; }
}
}
