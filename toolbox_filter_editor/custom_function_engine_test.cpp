// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "custom_function_engine.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using pj_custom_function::CustomFunctionEngine;
using pj_custom_function::OutputPoint;
using pj_custom_function::SeriesAccessor;

SeriesAccessor makeSeries(std::vector<double> ts, std::vector<double> vs) {
  SeriesAccessor s;
  s.timestamps = std::move(ts);
  s.values = std::move(vs);
  return s;
}

TEST(CustomFunctionEngine, SingleValueReturn) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("", "return value * 2.0", 0), "");

  auto main = makeSeries({1.0, 2.0, 3.0}, {10.0, 20.0, 30.0});
  std::vector<OutputPoint> out;
  ASSERT_EQ(engine.evaluate(main, {}, -1e18, out), "");

  ASSERT_EQ(out.size(), 3u);
  EXPECT_DOUBLE_EQ(out[0].t, 1.0);
  EXPECT_DOUBLE_EQ(out[0].v, 20.0);
  EXPECT_DOUBLE_EQ(out[2].v, 60.0);
}

TEST(CustomFunctionEngine, TimeValuePairReturn) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("", "return time + 100.0, value + 1.0", 0), "");

  auto main = makeSeries({1.0, 2.0}, {10.0, 20.0});
  std::vector<OutputPoint> out;
  ASSERT_EQ(engine.evaluate(main, {}, -1e18, out), "");

  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[0].t, 101.0);
  EXPECT_DOUBLE_EQ(out[0].v, 11.0);
  EXPECT_DOUBLE_EQ(out[1].t, 102.0);
  EXPECT_DOUBLE_EQ(out[1].v, 21.0);
}

TEST(CustomFunctionEngine, TableOfPairsReturn) {
  CustomFunctionEngine engine;
  // Emit two points per input sample.
  ASSERT_EQ(engine.compile("", "return { {time, value}, {time + 0.5, value * 10.0} }", 0), "");

  auto main = makeSeries({1.0}, {2.0});
  std::vector<OutputPoint> out;
  ASSERT_EQ(engine.evaluate(main, {}, -1e18, out), "");

  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[0].t, 1.0);
  EXPECT_DOUBLE_EQ(out[0].v, 2.0);
  EXPECT_DOUBLE_EQ(out[1].t, 1.5);
  EXPECT_DOUBLE_EQ(out[1].v, 20.0);
}

TEST(CustomFunctionEngine, AdditionalSourcesSampledByTime) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("", "return value + v1 + v2", 2), "");

  auto main = makeSeries({1.0, 2.0, 3.0}, {1.0, 2.0, 3.0});
  auto v1 = makeSeries({1.0, 2.0, 3.0}, {10.0, 20.0, 30.0});
  auto v2 = makeSeries({1.0, 2.0, 3.0}, {100.0, 200.0, 300.0});

  std::vector<OutputPoint> out;
  ASSERT_EQ(engine.evaluate(main, {&v1, &v2}, -1e18, out), "");

  ASSERT_EQ(out.size(), 3u);
  EXPECT_DOUBLE_EQ(out[0].v, 111.0);
  EXPECT_DOUBLE_EQ(out[1].v, 222.0);
  EXPECT_DOUBLE_EQ(out[2].v, 333.0);
}

TEST(CustomFunctionEngine, GlobalCodeIsAvailableToFunction) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("scale = 3.0", "return value * scale", 0), "");

  auto main = makeSeries({0.0}, {7.0});
  std::vector<OutputPoint> out;
  ASSERT_EQ(engine.evaluate(main, {}, -1e18, out), "");

  ASSERT_EQ(out.size(), 1u);
  EXPECT_DOUBLE_EQ(out[0].v, 21.0);
}

TEST(CustomFunctionEngine, IncrementalAfterTimestamp) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("", "return value", 0), "");

  auto main = makeSeries({1.0, 2.0, 3.0, 4.0}, {10.0, 20.0, 30.0, 40.0});
  std::vector<OutputPoint> out;
  // Only samples strictly after t=2.0 should be emitted.
  ASSERT_EQ(engine.evaluate(main, {}, 2.0, out), "");

  ASSERT_EQ(out.size(), 2u);
  EXPECT_DOUBLE_EQ(out[0].t, 3.0);
  EXPECT_DOUBLE_EQ(out[1].t, 4.0);
}

TEST(CustomFunctionEngine, CompileErrorReturnsMessage) {
  CustomFunctionEngine engine;
  std::string err = engine.compile("", "this is not valid lua )(", 0);
  EXPECT_FALSE(err.empty());
}

TEST(CustomFunctionEngine, RuntimeErrorReturnsMessage) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("", "error('boom')", 0), "");

  auto main = makeSeries({1.0}, {1.0});
  std::vector<OutputPoint> out;
  std::string err = engine.evaluate(main, {}, -1e18, out);
  EXPECT_FALSE(err.empty());
}

TEST(CustomFunctionEngine, EmptyAdditionalSourceYieldsNaN) {
  CustomFunctionEngine engine;
  ASSERT_EQ(engine.compile("", "if v1 ~= v1 then return -1.0 else return v1 end", 1), "");

  auto main = makeSeries({1.0}, {5.0});
  SeriesAccessor empty;  // no samples -> NaN
  std::vector<OutputPoint> out;
  ASSERT_EQ(engine.evaluate(main, {&empty}, -1e18, out), "");

  ASSERT_EQ(out.size(), 1u);
  EXPECT_DOUBLE_EQ(out[0].v, -1.0);
}

}  // namespace
