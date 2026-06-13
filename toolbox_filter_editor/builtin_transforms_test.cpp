// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plugins/sdk/builtin_transforms.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "pj_plugins/sdk/filter_transform_factory.hpp"

namespace {

using namespace PJ::sdk;  // NOLINT

// Helper: apply a transform to (xs, ys) pairs and return output points.
std::vector<Point2> apply(FilterTransform& t, std::vector<double> xs, std::vector<double> ys) {
  std::vector<Point2> in;
  in.reserve(xs.size());
  for (size_t i = 0; i < xs.size(); ++i) {
    in.push_back({xs[i], ys[i]});
  }
  return t.applyBatch(in);
}

TEST(Transforms, Absolute) {
  AbsoluteTransform t;
  auto out = apply(t, {0, 1, 2}, {-3, 4, -5});
  ASSERT_EQ(out.size(), 3u);
  EXPECT_DOUBLE_EQ(out[0].y, 3.0);
  EXPECT_DOUBLE_EQ(out[2].y, 5.0);
}

TEST(Transforms, ScaleAndOffset) {
  ScaleTransform t;
  t.value_scale = 2.0;
  t.value_offset = 1.0;
  t.time_offset = 10.0;
  auto out = apply(t, {0, 1}, {3, 4});
  EXPECT_DOUBLE_EQ(out[0].x, 10.0);
  EXPECT_DOUBLE_EQ(out[0].y, 7.0);
  EXPECT_DOUBLE_EQ(out[1].y, 9.0);
}

TEST(Transforms, DerivativeActualDt) {
  DerivativeTransform t;
  auto out = apply(t, {0, 1, 2}, {0, 10, 30});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[0].y, 10.0);
  EXPECT_DOUBLE_EQ(out[1].y, 20.0);
}

TEST(Transforms, IntegralTrapezoid) {
  IntegralTransform t;
  auto out = apply(t, {0, 1, 2}, {0, 2, 4});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[0].y, 1.0);
  EXPECT_DOUBLE_EQ(out[1].y, 4.0);
}

TEST(Transforms, MovingAverageWindow) {
  MovingAverageTransform t;
  t.window = 2;
  auto out = apply(t, {0, 1, 2, 3}, {2, 4, 6, 8});
  ASSERT_EQ(out.size(), 4u);
  EXPECT_DOUBLE_EQ(out[0].y, 2.0);  // padded
  EXPECT_DOUBLE_EQ(out[1].y, 3.0);  // (2+4)/2
  EXPECT_DOUBLE_EQ(out[3].y, 7.0);  // (6+8)/2
}

TEST(Transforms, MovingRMS) {
  MovingRMSTransform t;
  t.window = 2;
  auto out = apply(t, {0, 1}, {3, 4});
  EXPECT_DOUBLE_EQ(out[0].y, 3.0);
  EXPECT_DOUBLE_EQ(out[1].y, std::sqrt((9.0 + 16.0) / 2.0));
}

TEST(Transforms, BinaryFilterGreater) {
  BinaryFilterTransform t;
  t.op = BinaryOp::kGreater;
  t.a = 5.0;
  auto out = apply(t, {0, 1, 2}, {3, 5, 7});
  EXPECT_DOUBLE_EQ(out[0].y, 0.0);
  EXPECT_DOUBLE_EQ(out[1].y, 0.0);
  EXPECT_DOUBLE_EQ(out[2].y, 1.0);
}

TEST(Transforms, BinaryFilterRange) {
  BinaryFilterTransform t;
  t.op = BinaryOp::kRange;
  t.a = 2.0;
  t.b = 6.0;
  auto out = apply(t, {0, 1, 2}, {1, 4, 7});
  EXPECT_DOUBLE_EQ(out[0].y, 0.0);
  EXPECT_DOUBLE_EQ(out[1].y, 1.0);
  EXPECT_DOUBLE_EQ(out[2].y, 0.0);
}

TEST(Transforms, TimeSincePrevious) {
  TimeSincePreviousTransform t;
  auto out = apply(t, {0, 2, 5}, {9, 9, 9});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[0].y, 2.0);
  EXPECT_DOUBLE_EQ(out[1].y, 3.0);
}

TEST(Transforms, SamplesCounter) {
  SamplesCounterTransform t;
  t.samples_ms = 2000;
  auto out = apply(t, {0, 1, 2, 3}, {0, 0, 0, 0});
  ASSERT_EQ(out.size(), 4u);
  EXPECT_DOUBLE_EQ(out[0].y, 0.0);
  EXPECT_DOUBLE_EQ(out[2].y, 2.0);
}

TEST(Transforms, NoneIsPassthrough) {
  NoneTransform t;
  auto out = apply(t, {0, 1}, {5, 6});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[1].y, 6.0);
}

TEST(Transforms, FactoryRegistration) {
  // The host owns the factory now (one source of truth); for the test we just
  // exercise the same registration shape the plugin's loaderInit uses.
  FilterTransformFactory f;
  f.registerTransform(
      "scale", []() -> FilterTransform* { return new ScaleTransform{}; }, [](FilterTransform* p) noexcept { delete p; },
      {});
  f.registerTransform(
      "moving_average", []() -> FilterTransform* { return new MovingAverageTransform{}; },
      [](FilterTransform* p) noexcept { delete p; }, {});

  const auto ids = f.registeredIds();
  EXPECT_FALSE(ids.empty());
  for (const auto& id : ids) {
    auto t = f.create(id);
    EXPECT_NE(t, nullptr) << "Could not create: " << id;
  }
}

TEST(Transforms, StreamSafeFlags) {
  // Known stream-safe
  EXPECT_TRUE(AbsoluteTransform{}.isStreamSafe());
  EXPECT_TRUE(ScaleTransform{}.isStreamSafe());
  EXPECT_TRUE(DerivativeTransform{}.isStreamSafe());
  EXPECT_TRUE(MovingAverageTransform{}.isStreamSafe());
  EXPECT_TRUE(MovingRMSTransform{}.isStreamSafe());
  EXPECT_TRUE(MovingVarianceTransform{}.isStreamSafe());
  EXPECT_TRUE(OutlierRemovalTransform{}.isStreamSafe());
  EXPECT_TRUE(BinaryFilterTransform{}.isStreamSafe());
  EXPECT_TRUE(TimeSincePreviousTransform{}.isStreamSafe());
  // Known non-safe (need full history)
  EXPECT_FALSE(IntegralTransform{}.isStreamSafe());
  EXPECT_FALSE(SamplesCounterTransform{}.isStreamSafe());
}

// ---------------------------------------------------------------------------
// Streaming equivalence: calculateNextPoint matches applyBatch
// ---------------------------------------------------------------------------

void expectStreamingMatchesBatch(FilterTransform& t_batch, FilterTransform& t_stream) {
  std::vector<Point2> full;
  for (int i = 0; i < 30; ++i) {
    full.push_back({static_cast<double>(i), std::sin(i * 0.3) * 5.0 + static_cast<double>(i % 5)});
  }
  // Batch
  const auto batch = t_batch.applyBatch(full);
  // Streaming point-by-point
  t_stream.reset();
  std::vector<Point2> streamed;
  for (const auto& p : full) {
    if (auto r = t_stream.calculateNextPoint(p)) {
      streamed.push_back(*r);
    }
  }
  ASSERT_EQ(batch.size(), streamed.size());
  for (size_t i = 0; i < batch.size(); ++i) {
    EXPECT_NEAR(batch[i].x, streamed[i].x, 1e-9) << "x mismatch at " << i;
    EXPECT_NEAR(batch[i].y, streamed[i].y, 1e-9) << "y mismatch at " << i;
  }
}

TEST(Transforms, StreamingMatchesBatch) {
  {
    AbsoluteTransform a, b;
    expectStreamingMatchesBatch(a, b);
  }
  {
    DerivativeTransform a, b;
    expectStreamingMatchesBatch(a, b);
  }
  {
    IntegralTransform a, b;
    expectStreamingMatchesBatch(a, b);
  }
  {
    MovingAverageTransform a, b;
    a.window = b.window = 5;
    expectStreamingMatchesBatch(a, b);
  }
  {
    MovingRMSTransform a, b;
    a.window = b.window = 5;
    expectStreamingMatchesBatch(a, b);
  }
  {
    TimeSincePreviousTransform a, b;
    expectStreamingMatchesBatch(a, b);
  }
  {
    BinaryFilterTransform a, b;
    a.op = b.op = BinaryOp::kGreater;
    a.a = b.a = 2.0;
    expectStreamingMatchesBatch(a, b);
  }
}

}  // namespace
