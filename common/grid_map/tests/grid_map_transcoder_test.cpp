// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <pj_base/builtin/grid_map_codec.hpp>
#include <pj_grid_map/grid_map_transcoder.hpp>
#include <string>
#include <vector>

namespace {

using PJ::grid_map::GridMapMessage;
using PJ::grid_map::LayerArray;
using PJ::grid_map::MultiArrayDimension;
using PJ::grid_map::transcodeGridMap;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

PJ::Span<const float> span(const std::vector<float>& v) {
  return PJ::Span<const float>(v.data(), v.size());
}

/// grid_map layout for a size_x x size_y map: dim[0] = "column_index"
/// (size_y, stride = cells), dim[1] = "row_index" (size_x, stride = size_x).
LayerArray layer(const std::vector<float>& data, uint32_t size_x, uint32_t size_y, uint32_t data_offset = 0) {
  LayerArray out;
  out.dims = {
      {.label = "column_index", .size = size_y, .stride = size_x * size_y},
      {.label = "row_index", .size = size_x, .stride = size_x},
  };
  out.data_offset = data_offset;
  out.data = span(data);
  return out;
}

/// The 3x2 golden map: size_x = 3, size_y = 2, one layer whose column-major
/// storage holds its own flat index (0..5), resolution 0.5 centered at (10, 20).
struct Golden {
  std::vector<float> ids = {0, 1, 2, 3, 4, 5};
  GridMapMessage msg;
  Golden() {
    msg.resolution = 0.5;
    msg.length_x = 1.5;
    msg.length_y = 1.0;
    msg.center_x = 10.0;
    msg.center_y = 20.0;
    msg.layers = {"elevation"};
    msg.data = {layer(ids, 3, 2)};
  }
};

/// Reads field `field` of cell (c, r) from a transcoded grid.
float cell(const PJ::sdk::GridMap& g, uint32_t c, uint32_t r, size_t field = 0) {
  float v = 0.0f;
  std::memcpy(&v, g.data.data() + r * g.row_stride + c * g.cell_stride + g.fields[field].offset, sizeof(float));
  return v;
}

std::vector<float> rowMajor(const PJ::sdk::GridMap& g, size_t field = 0) {
  std::vector<float> out;
  for (uint32_t r = 0; r < g.row_count; ++r) {
    for (uint32_t c = 0; c < g.column_count; ++c) {
      out.push_back(cell(g, c, r, field));
    }
  }
  return out;
}

TEST(GridMapTranscoderTest, GoldenAxisFlipAndGeometry) {
  Golden golden;
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  const auto& g = *res;

  EXPECT_EQ(g.column_count, 3u);
  EXPECT_EQ(g.row_count, 2u);
  EXPECT_EQ(g.cell_stride, 4u);
  EXPECT_EQ(g.row_stride, 12u);
  EXPECT_DOUBLE_EQ(g.cell_size.x, 0.5);
  EXPECT_DOUBLE_EQ(g.cell_size.y, 0.5);
  // Corner of cell (0,0): center - size*res/2, z = 0, identity orientation.
  EXPECT_DOUBLE_EQ(g.origin.position.x, 10.0 - 0.75);
  EXPECT_DOUBLE_EQ(g.origin.position.y, 20.0 - 0.5);
  EXPECT_DOUBLE_EQ(g.origin.position.z, 0.0);
  EXPECT_EQ(g.origin.orientation, PJ::sdk::Quaternion{});
  ASSERT_EQ(g.fields.size(), 1u);
  EXPECT_EQ(g.fields[0].name, "elevation");
  EXPECT_EQ(g.fields[0].offset, 0u);
  EXPECT_EQ(g.fields[0].datatype, PJ::sdk::PointField::Datatype::kFloat32);
  EXPECT_EQ(g.fields[0].count, 1u);
  ASSERT_EQ(g.data.size(), 24u);
  ASSERT_NE(g.anchor, nullptr);

  // grid_map index (0,0) is the +x/+y corner: canonical cell (c, r) reads
  // matrix element (size_x-1-c, size_y-1-r), stored column-major at j*size_x+i.
  EXPECT_EQ(rowMajor(g), (std::vector<float>{5, 4, 3, 2, 1, 0}));
  EXPECT_TRUE(PJ::validateGridMap(g).has_value());
}

TEST(GridMapTranscoderTest, OuterStartIndexRotatesColumns) {
  Golden golden;
  golden.msg.outer_start_index = 1;
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_EQ(rowMajor(*res), (std::vector<float>{3, 5, 4, 0, 2, 1}));
}

TEST(GridMapTranscoderTest, InnerStartIndexRotatesRows) {
  Golden golden;
  golden.msg.inner_start_index = 1;
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_EQ(rowMajor(*res), (std::vector<float>{2, 1, 0, 5, 4, 3}));
}

TEST(GridMapTranscoderTest, CombinedStartIndices) {
  Golden golden;
  golden.msg.outer_start_index = 1;
  golden.msg.inner_start_index = 1;
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_EQ(rowMajor(*res), (std::vector<float>{0, 2, 1, 3, 5, 4}));
}

TEST(GridMapTranscoderTest, DataOffsetIsHonoredAndTrailingElementsTolerated) {
  Golden golden;
  const std::vector<float> padded = {-1, -1, 0, 1, 2, 3, 4, 5, 99, 99};
  golden.msg.data = {layer(padded, 3, 2, 2)};
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_EQ(rowMajor(*res), (std::vector<float>{5, 4, 3, 2, 1, 0}));
}

TEST(GridMapTranscoderTest, TruncatedArrayRejected) {
  Golden golden;
  const std::vector<float> five = {0, 1, 2, 3, 4};
  golden.msg.data = {layer(five, 3, 2)};
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
  // data_offset pushes the window past the end even though 6 elements exist.
  golden.msg.data = {layer(golden.ids, 3, 2, 1)};
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
}

TEST(GridMapTranscoderTest, MultipleLayersInterleavePerCell) {
  Golden golden;
  const std::vector<float> tens = {10, 11, 12, 13, 14, 15};
  golden.msg.layers = {"elevation", "variance"};
  golden.msg.data = {layer(golden.ids, 3, 2), layer(tens, 3, 2)};
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  const auto& g = *res;
  EXPECT_EQ(g.cell_stride, 8u);
  EXPECT_EQ(g.row_stride, 24u);
  ASSERT_EQ(g.fields.size(), 2u);
  EXPECT_EQ(g.fields[1].name, "variance");
  EXPECT_EQ(g.fields[1].offset, 4u);
  EXPECT_EQ(rowMajor(g, 0), (std::vector<float>{5, 4, 3, 2, 1, 0}));
  EXPECT_EQ(rowMajor(g, 1), (std::vector<float>{15, 14, 13, 12, 11, 10}));
}

TEST(GridMapTranscoderTest, BasicLayerInvalidityBlanksWholeCell) {
  Golden golden;
  // Storage index 4 (-> canonical cell c=1, r=0) is NaN in the basic layer; the
  // other layer is finite everywhere, yet the cell must be NaN in BOTH fields.
  const std::vector<float> validity = {1, 1, 1, 1, kNaN, 1};
  golden.msg.layers = {"elevation", "valid"};
  golden.msg.basic_layers = {"valid"};
  golden.msg.data = {layer(golden.ids, 3, 2), layer(validity, 3, 2)};
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  const auto& g = *res;
  EXPECT_TRUE(std::isnan(cell(g, 1, 0, 0)));
  EXPECT_TRUE(std::isnan(cell(g, 1, 0, 1)));
  // Neighbours untouched.
  EXPECT_EQ(cell(g, 0, 0, 0), 5.0f);
  EXPECT_EQ(cell(g, 2, 0, 0), 3.0f);
  EXPECT_EQ(cell(g, 0, 0, 1), 1.0f);

  // Without basic_layers, a NaN is just a NaN in its own layer.
  golden.msg.basic_layers.clear();
  res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_EQ(cell(*res, 1, 0, 0), 4.0f);
  EXPECT_TRUE(std::isnan(cell(*res, 1, 0, 1)));
}

TEST(GridMapTranscoderTest, InfInBasicLayerAlsoInvalidates) {
  Golden golden;
  const std::vector<float> validity = {std::numeric_limits<float>::infinity(), 1, 1, 1, 1, 1};
  golden.msg.layers = {"elevation", "valid"};
  golden.msg.basic_layers = {"valid"};
  golden.msg.data = {layer(golden.ids, 3, 2), layer(validity, 3, 2)};
  auto res = transcodeGridMap(golden.msg);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_TRUE(std::isnan(cell(*res, 2, 1, 0)));  // storage 0 -> (c=2, r=1)
}

TEST(GridMapTranscoderTest, MalformedDimsRejected) {
  Golden golden;
  auto with_dims = [&](std::vector<MultiArrayDimension> dims) {
    Golden g;
    g.msg.data[0].dims = std::move(dims);
    return transcodeGridMap(g.msg).has_value();
  };
  EXPECT_FALSE(with_dims({}));
  EXPECT_FALSE(with_dims({{"column_index", 2, 6}}));
  EXPECT_FALSE(with_dims({{"column_index", 2, 6}, {"row_index", 3, 3}, {"channel", 1, 1}}));
  // Reversed (row-major) labelling is not grid_map's layout.
  EXPECT_FALSE(with_dims({{"row_index", 3, 6}, {"column_index", 2, 2}}));
  EXPECT_FALSE(with_dims({{"height", 2, 6}, {"width", 3, 3}}));
  // Zero sizes.
  EXPECT_FALSE(with_dims({{"column_index", 0, 0}, {"row_index", 3, 3}}));
  EXPECT_FALSE(with_dims({{"column_index", 2, 0}, {"row_index", 0, 0}}));
  // The golden layout itself still passes.
  EXPECT_TRUE(with_dims({{"column_index", 2, 6}, {"row_index", 3, 3}}));
}

TEST(GridMapTranscoderTest, LayersWithDifferentLayoutsRejected) {
  Golden golden;
  golden.msg.layers = {"a", "b"};
  golden.msg.data = {layer(golden.ids, 3, 2), layer(golden.ids, 2, 3)};
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
  // Same sizes, different stride.
  golden.msg.data = {layer(golden.ids, 3, 2), layer(golden.ids, 3, 2)};
  golden.msg.data[1].dims[1].stride = 1;
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
}

TEST(GridMapTranscoderTest, FirstLayerStridesCheckedAgainstLayout) {
  auto with_strides = [](uint32_t column_stride, uint32_t row_stride) {
    Golden g;
    g.msg.data[0].dims = {{"column_index", 2, column_stride}, {"row_index", 3, row_stride}};
    return transcodeGridMap(g.msg).has_value();
  };
  EXPECT_FALSE(with_strides(1, 1)) << "strides contradict the column-major layout";
  EXPECT_FALSE(with_strides(6, 0)) << "only one stride unspecified";
  EXPECT_TRUE(with_strides(0, 0)) << "both strides unspecified";
  EXPECT_TRUE(with_strides(6, 3)) << "grid_map's own strides";
}

TEST(GridMapTranscoderTest, StrideOverflowRejectedBeforeAllocation) {
  // 2^24 x 1 cells (at the cap) x 64 float32 layers: row_stride would be 2^32,
  // not representable in the SDK's uint32 strides. The arrays are empty so a
  // pass would fail later on length; the stride check must fire first (and
  // never allocate the 4 GiB output, so this test stays instant).
  Golden g;
  const uint32_t size_x = 1u << 24;
  g.msg.resolution = 1.0;
  g.msg.length_x = static_cast<double>(size_x);
  g.msg.length_y = 1.0;
  g.msg.layers.clear();
  g.msg.data.clear();
  for (size_t i = 0; i < 64; ++i) {
    g.msg.layers.push_back("l" + std::to_string(i));
    g.msg.data.push_back(layer({}, size_x, 1));
  }
  auto res = transcodeGridMap(g.msg);
  ASSERT_FALSE(res.has_value());
  EXPECT_NE(res.error().find("stride"), std::string::npos) << res.error();
}

TEST(GridMapTranscoderTest, NonFiniteLengthQuotientRejected) {
  Golden g;
  g.msg.resolution = 1e-300;
  g.msg.length_x = 1e300;  // quotient overflows to inf: no llround on it
  EXPECT_FALSE(transcodeGridMap(g.msg).has_value());
  Golden h;
  h.msg.resolution = 1e-300;
  h.msg.length_x = 3e-300;
  h.msg.length_y = 1e300;
  EXPECT_FALSE(transcodeGridMap(h.msg).has_value());
}

TEST(GridMapTranscoderTest, NonIntegralLengthRejected) {
  Golden golden;
  golden.msg.length_x = 1.6;  // 1.6 / 0.5 = 3.2 -> rounds to 3 == size_x, tolerated
  EXPECT_TRUE(transcodeGridMap(golden.msg).has_value());
  golden.msg.length_x = 2.0;  // 4 cells declared by length, 3 by the array
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
  golden.msg.length_x = 1.5;
  golden.msg.length_y = 1.5;  // 3 != size_y (2)
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
}

TEST(GridMapTranscoderTest, BadResolutionOrLengthsRejected) {
  for (double bad : {0.0, -0.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    Golden g;
    g.msg.resolution = bad;
    EXPECT_FALSE(transcodeGridMap(g.msg).has_value());
    Golden h;
    h.msg.length_x = bad;
    EXPECT_FALSE(transcodeGridMap(h.msg).has_value());
    Golden k;
    k.msg.length_y = bad;
    EXPECT_FALSE(transcodeGridMap(k.msg).has_value());
  }
}

TEST(GridMapTranscoderTest, StartIndexOutOfRangeRejected) {
  Golden golden;
  golden.msg.outer_start_index = 3;  // size_x = 3
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
  golden.msg.outer_start_index = 2;
  golden.msg.inner_start_index = 2;  // size_y = 2
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value());
  golden.msg.inner_start_index = 1;
  EXPECT_TRUE(transcodeGridMap(golden.msg).has_value());
}

TEST(GridMapTranscoderTest, LayerNameProblemsRejected) {
  Golden golden;
  golden.msg.layers = {"elevation", "elevation"};
  golden.msg.data = {layer(golden.ids, 3, 2), layer(golden.ids, 3, 2)};
  EXPECT_FALSE(transcodeGridMap(golden.msg).has_value()) << "duplicate names";

  Golden basic;
  basic.msg.basic_layers = {"not_a_layer"};
  EXPECT_FALSE(transcodeGridMap(basic.msg).has_value()) << "basic layer missing from layers";

  Golden count;
  count.msg.layers = {"elevation", "variance"};
  EXPECT_FALSE(transcodeGridMap(count.msg).has_value()) << "layer count != data count";

  Golden empty;
  empty.msg.layers.clear();
  empty.msg.data.clear();
  EXPECT_FALSE(transcodeGridMap(empty.msg).has_value()) << "empty layer list";
}

TEST(GridMapTranscoderTest, CapsRejectedBeforeAllocation) {
  // 4097 x 4096 cells > kMaxCells; the arrays are deliberately empty, so a
  // pass would fail later on length instead — the cap must fire first and
  // the assertion below distinguishes the two by message.
  Golden cells;
  cells.msg.resolution = 1.0;
  cells.msg.length_x = 4097.0;
  cells.msg.length_y = 4096.0;
  cells.msg.data = {layer({}, 4097, 4096)};
  auto res = transcodeGridMap(cells.msg);
  ASSERT_FALSE(res.has_value());
  EXPECT_NE(res.error().find("cells"), std::string::npos) << res.error();

  Golden layers;
  layers.msg.layers.clear();
  layers.msg.data.clear();
  for (size_t i = 0; i < PJ::grid_map::kMaxLayers + 1; ++i) {
    layers.msg.layers.push_back("l" + std::to_string(i));
    layers.msg.data.push_back(layer(layers.ids, 3, 2));
  }
  res = transcodeGridMap(layers.msg);
  ASSERT_FALSE(res.has_value());
  EXPECT_NE(res.error().find("layers"), std::string::npos) << res.error();
}

// ---------------------------------------------------------------------------
// Foxglove Grid helpers
// ---------------------------------------------------------------------------

TEST(FoxgloveGridTest, PackedElementTypeMapping) {
  using D = PJ::sdk::PointField::Datatype;
  using PJ::grid_map::foxgloveNumericTypeToPointField;
  EXPECT_EQ(foxgloveNumericTypeToPointField(0), D::kUnknown);
  EXPECT_EQ(foxgloveNumericTypeToPointField(1), D::kUint8);
  EXPECT_EQ(foxgloveNumericTypeToPointField(2), D::kInt8);
  EXPECT_EQ(foxgloveNumericTypeToPointField(3), D::kUint16);
  EXPECT_EQ(foxgloveNumericTypeToPointField(4), D::kInt16);
  EXPECT_EQ(foxgloveNumericTypeToPointField(5), D::kUint32);
  EXPECT_EQ(foxgloveNumericTypeToPointField(6), D::kInt32);
  EXPECT_EQ(foxgloveNumericTypeToPointField(7), D::kFloat32);
  EXPECT_EQ(foxgloveNumericTypeToPointField(8), D::kFloat64);
  EXPECT_EQ(foxgloveNumericTypeToPointField(9), D::kUnknown);
}

PJ::sdk::GridMap foxgloveGrid(uint32_t columns, uint32_t cell_stride, uint32_t row_stride, size_t data_size) {
  static std::vector<uint8_t> storage;
  storage.assign(data_size, 0);
  PJ::sdk::GridMap g;
  g.column_count = columns;
  g.cell_stride = cell_stride;
  g.row_stride = row_stride;
  g.cell_size = {.x = 0.1, .y = 0.1};
  g.fields = {{.name = "elevation", .offset = 0, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1}};
  g.data = PJ::Span<const uint8_t>(storage.data(), storage.size());
  return g;
}

TEST(FoxgloveGridTest, FinalizeDerivesRowCount) {
  auto g = foxgloveGrid(3, 4, 16, 48);  // row_stride padded past 3*4
  auto res = PJ::grid_map::finalizeFoxgloveGrid(g);
  ASSERT_TRUE(res.has_value()) << res.error();
  EXPECT_EQ(g.row_count, 3u);
}

TEST(FoxgloveGridTest, FinalizeEmptyDataIsZeroRows) {
  auto g = foxgloveGrid(3, 4, 12, 0);
  ASSERT_TRUE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value());
  EXPECT_EQ(g.row_count, 0u);
  // Even with a zero row_stride: nothing to index.
  g = foxgloveGrid(0, 0, 0, 0);
  g.fields.clear();
  EXPECT_TRUE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value());
}

TEST(FoxgloveGridTest, FinalizeRejections) {
  auto g = foxgloveGrid(3, 4, 0, 12);
  EXPECT_FALSE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value()) << "row_stride 0 with data";
  g = foxgloveGrid(3, 4, 12, 30);
  EXPECT_FALSE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value()) << "row-stride remainder";
  g = foxgloveGrid(4, 4, 12, 24);
  EXPECT_FALSE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value()) << "columns overflow the row";
  g = foxgloveGrid(3, 4, 12, 24);
  g.fields[0].datatype = PJ::sdk::PointField::Datatype::kUnknown;
  EXPECT_FALSE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value()) << "unknown datatype";
  g = foxgloveGrid(3, 4, 12, 24);
  g.fields[0].offset = 4;  // field ends past cell_stride
  EXPECT_FALSE(PJ::grid_map::finalizeFoxgloveGrid(g).has_value()) << "field past cell_stride";
}

}  // namespace
