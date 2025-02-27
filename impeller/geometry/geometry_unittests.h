// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <iostream>

#include "gtest/gtest.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"
#include "impeller/geometry/size.h"
#include "impeller/geometry/vector.h"
#include "impeller/geometry/vertices.h"

inline bool NumberNear(double a, double b) {
  static const double epsilon = 1e-3;
  return (a > (b - epsilon)) && (a < (b + epsilon));
}

inline ::testing::AssertionResult MatrixNear(impeller::Matrix a,
                                             impeller::Matrix b) {
  auto equal = NumberNear(a.m[0], b.m[0])       //
               && NumberNear(a.m[1], b.m[1])    //
               && NumberNear(a.m[2], b.m[2])    //
               && NumberNear(a.m[3], b.m[3])    //
               && NumberNear(a.m[4], b.m[4])    //
               && NumberNear(a.m[5], b.m[5])    //
               && NumberNear(a.m[6], b.m[6])    //
               && NumberNear(a.m[7], b.m[7])    //
               && NumberNear(a.m[8], b.m[8])    //
               && NumberNear(a.m[9], b.m[9])    //
               && NumberNear(a.m[10], b.m[10])  //
               && NumberNear(a.m[11], b.m[11])  //
               && NumberNear(a.m[12], b.m[12])  //
               && NumberNear(a.m[13], b.m[13])  //
               && NumberNear(a.m[14], b.m[14])  //
               && NumberNear(a.m[15], b.m[15]);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Matrixes are not equal.";
}

inline ::testing::AssertionResult QuaternionNear(impeller::Quaternion a,
                                                 impeller::Quaternion b) {
  auto equal = NumberNear(a.x, b.x) && NumberNear(a.y, b.y) &&
               NumberNear(a.z, b.z) && NumberNear(a.w, b.w);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Quaternions are not equal.";
}

inline ::testing::AssertionResult RectNear(impeller::Rect a, impeller::Rect b) {
  auto equal = NumberNear(a.origin.x, b.origin.x) &&
               NumberNear(a.origin.y, b.origin.y) &&
               NumberNear(a.size.width, b.size.width) &&
               NumberNear(a.size.height, b.size.height);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Rects are not equal.";
}

inline ::testing::AssertionResult ColorNear(impeller::Color a,
                                            impeller::Color b) {
  auto equal = NumberNear(a.red, b.red) && NumberNear(a.green, b.green) &&
               NumberNear(a.blue, b.blue) && NumberNear(a.alpha, b.alpha);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Colors are not equal.";
}

inline ::testing::AssertionResult PointNear(impeller::Point a,
                                            impeller::Point b) {
  auto equal = NumberNear(a.x, b.x) && NumberNear(a.y, b.y);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Points are not equal.";
}

inline ::testing::AssertionResult Vector3Near(impeller::Vector3 a,
                                              impeller::Vector3 b) {
  auto equal =
      NumberNear(a.x, b.x) && NumberNear(a.y, b.y) && NumberNear(a.z, b.z);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Vector3s are not equal.";
}

inline ::testing::AssertionResult Vector4Near(impeller::Vector4 a,
                                              impeller::Vector4 b) {
  auto equal = NumberNear(a.x, b.x) && NumberNear(a.y, b.y) &&
               NumberNear(a.z, b.z) && NumberNear(a.w, b.w);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Vector4s are not equal.";
}

inline ::testing::AssertionResult SizeNear(impeller::Size a, impeller::Size b) {
  auto equal = NumberNear(a.width, b.width) && NumberNear(a.height, b.height);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Sizes are not equal.";
}

inline ::testing::AssertionResult Array4Near(std::array<uint8_t, 4> a,
                                             std::array<uint8_t, 4> b) {
  auto equal = NumberNear(a[0], b[0]) && NumberNear(a[1], b[1]) &&
               NumberNear(a[2], b[2]) && NumberNear(a[3], b[3]);

  return equal ? ::testing::AssertionSuccess()
               : ::testing::AssertionFailure() << "Arrays are not equal.";
}

inline ::testing::AssertionResult ColorBufferNear(
    std::vector<uint8_t> a,
    std::vector<impeller::Color> b) {
  if (a.size() != b.size() * 4) {
    return ::testing::AssertionFailure()
           << "Color buffer length does not match";
  }
  for (auto i = 0u; i < b.size(); i++) {
    auto right = b[i].Premultiply().ToR8G8B8A8();
    auto j = i * 4;
    auto equal = NumberNear(a[j], right[0]) && NumberNear(a[j + 1], right[1]) &&
                 NumberNear(a[j + 2], right[2]) &&
                 NumberNear(a[j + 3], right[3]);
    if (!equal) {
      ::testing::AssertionFailure() << "Color buffers are not equal.";
    }
  }
  return ::testing::AssertionSuccess();
}

inline ::testing::AssertionResult ColorsNear(std::vector<impeller::Color> a,
                                             std::vector<impeller::Color> b) {
  if (a.size() != b.size()) {
    return ::testing::AssertionFailure() << "Colors length does not match";
  }
  for (auto i = 0u; i < b.size(); i++) {
    auto equal =
        NumberNear(a[i].red, b[i].red) && NumberNear(a[i].green, b[i].green) &&
        NumberNear(a[i].blue, b[i].blue) && NumberNear(a[i].alpha, b[i].alpha);

    if (!equal) {
      ::testing::AssertionFailure() << "Colors are not equal.";
    }
  }
  return ::testing::AssertionSuccess();
}

#define ASSERT_MATRIX_NEAR(a, b) ASSERT_PRED2(&::MatrixNear, a, b)
#define ASSERT_QUATERNION_NEAR(a, b) ASSERT_PRED2(&::QuaternionNear, a, b)
#define ASSERT_RECT_NEAR(a, b) ASSERT_PRED2(&::RectNear, a, b)
#define ASSERT_COLOR_NEAR(a, b) ASSERT_PRED2(&::ColorNear, a, b)
#define ASSERT_POINT_NEAR(a, b) ASSERT_PRED2(&::PointNear, a, b)
#define ASSERT_VECTOR3_NEAR(a, b) ASSERT_PRED2(&::Vector3Near, a, b)
#define ASSERT_VECTOR4_NEAR(a, b) ASSERT_PRED2(&::Vector4Near, a, b)
#define ASSERT_SIZE_NEAR(a, b) ASSERT_PRED2(&::SizeNear, a, b)
#define ASSERT_ARRAY_4_NEAR(a, b) ASSERT_PRED2(&::Array4Near, a, b)
#define ASSERT_COLOR_BUFFER_NEAR(a, b) ASSERT_PRED2(&::ColorBufferNear, a, b)
#define ASSERT_COLORS_NEAR(a, b) ASSERT_PRED2(&::ColorsNear, a, b)
