// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Routing is driven purely by the server ontology tag (mosaico:properties),
// never by guessing at the column structure. This guards the fix for
// `/laser/.../points`: point clouds carry data + width + height just like raw
// images, so any structural heuristic false-positives. The rule here is simple
// — read the tag; if it isn't an image tag, it isn't routed to the 2D view.

#include "../src/ontology_routing.h"

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <memory>

namespace {

using mosaico::isCanonicalObjectOntology;
using mosaico::isFuturesPointCloudOntology;
using mosaico::isGridCellsOntology;
using mosaico::isImageOntology;
using mosaico::isLaserScanOntology;
using mosaico::isOccupancyGridOntology;
using mosaico::isPointCloudOntology;
using mosaico::isPoseOntology;
using mosaico::isTransformOntology;
using mosaico::resolveOntologyTag;

// A schema carrying a server ontology tag in mosaico:properties.
std::shared_ptr<arrow::Schema> taggedSchema(const std::string& ontology_tag) {
  auto md = arrow::KeyValueMetadata::Make({"mosaico:properties"}, {R"({"ontology_tag":")" + ontology_tag + R"("})"});
  return arrow::schema({arrow::field("data", arrow::binary())}, md);
}

// An image-SHAPED schema (data + width + height) with NO server tag — the exact
// trap a structural heuristic falls into (point clouds look identical).
std::shared_ptr<arrow::Schema> untaggedImageShapedSchema() {
  return arrow::schema({
      arrow::field("data", arrow::binary()),
      arrow::field("width", arrow::int32()),
      arrow::field("height", arrow::int32()),
  });
}

TEST(OntologyRouting, CachedServerTagWins) {
  // The tag cached from getTopicMetadata short-circuits everything.
  EXPECT_EQ(resolveOntologyTag(untaggedImageShapedSchema(), "point_cloud"), "point_cloud");
  EXPECT_FALSE(isImageOntology(resolveOntologyTag(untaggedImageShapedSchema(), "point_cloud")));
  EXPECT_TRUE(isImageOntology(resolveOntologyTag(untaggedImageShapedSchema(), "image")));
}

TEST(OntologyRouting, SchemaMetadataTagDrivesRouting) {
  EXPECT_EQ(resolveOntologyTag(taggedSchema("image"), ""), "image");
  EXPECT_TRUE(isImageOntology(resolveOntologyTag(taggedSchema("image"), "")));
  EXPECT_EQ(resolveOntologyTag(taggedSchema("compressed_image"), ""), "compressed_image");
  EXPECT_TRUE(isImageOntology(resolveOntologyTag(taggedSchema("compressed_image"), "")));
  // A point cloud is explicitly tagged and never treated as an image.
  EXPECT_EQ(resolveOntologyTag(taggedSchema("point_cloud"), ""), "point_cloud");
  EXPECT_FALSE(isImageOntology(resolveOntologyTag(taggedSchema("point_cloud"), "")));
}

TEST(OntologyRouting, ImageShapedColumnsWithoutTagAreNotGuessedAsImage) {
  // No cached tag, no schema metadata: we do NOT infer "image" from the columns
  // (this is exactly what misrouted point clouds). It resolves to "" → scalar.
  EXPECT_EQ(resolveOntologyTag(untaggedImageShapedSchema(), ""), "");
  EXPECT_FALSE(isImageOntology(resolveOntologyTag(untaggedImageShapedSchema(), "")));
}

TEST(OntologyRouting, RawVsCompressedDistinguishedByTag) {
  // The exact strings the pull path branches on (tag == "compressed_image").
  EXPECT_EQ(resolveOntologyTag(taggedSchema("image"), ""), "image");
  EXPECT_EQ(resolveOntologyTag(taggedSchema("compressed_image"), ""), "compressed_image");
}

TEST(OntologyRouting, NullSchemaAndEmptyTagYieldEmpty) {
  EXPECT_EQ(resolveOntologyTag(nullptr, ""), "");
  EXPECT_FALSE(isImageOntology(resolveOntologyTag(nullptr, "")));
}

// The 3D-object ontologies each route to exactly one classifier, and all of
// them count as canonical objects (vs falling through to the scalar pipeline).
TEST(OntologyRouting, NewObjectOntologiesClassify) {
  EXPECT_TRUE(isTransformOntology("transform"));
  EXPECT_TRUE(isTransformOntology("frame_transform"));
  EXPECT_TRUE(isOccupancyGridOntology("occupancy_grid"));
  EXPECT_TRUE(isLaserScanOntology("laser_scan"));
  EXPECT_TRUE(isGridCellsOntology("grid_cells"));
  for (const std::string tag : {"lidar", "radar", "rgbd_camera", "tof_camera", "stereo_camera"}) {
    EXPECT_TRUE(isFuturesPointCloudOntology(tag)) << tag;
    EXPECT_TRUE(isCanonicalObjectOntology(tag)) << tag;
  }
  for (const std::string tag : {"transform", "frame_transform", "occupancy_grid", "laser_scan", "grid_cells"}) {
    EXPECT_TRUE(isCanonicalObjectOntology(tag)) << tag;
  }
}

// The classifiers are mutually exclusive and don't bleed into the existing
// image/point_cloud/pose routes (or vice-versa).
TEST(OntologyRouting, NewObjectOntologiesAreDisjoint) {
  EXPECT_FALSE(isTransformOntology("occupancy_grid"));
  EXPECT_FALSE(isOccupancyGridOntology("grid_cells"));
  EXPECT_FALSE(isLaserScanOntology("lidar"));  // futures cloud, not a scan
  EXPECT_FALSE(isFuturesPointCloudOntology("point_cloud2"));
  EXPECT_FALSE(isPointCloudOntology("laser_scan"));  // packed vs polar-expanded
  EXPECT_FALSE(isPoseOntology("transform"));
  EXPECT_FALSE(isImageOntology("occupancy_grid"));
  // Unknown tags stay scalar.
  EXPECT_FALSE(isCanonicalObjectOntology("imu"));
  EXPECT_FALSE(isCanonicalObjectOntology(""));
}

}  // namespace

TEST(OntologyRouting, CanonicalMetadataMatchesTheRoutingPredicates) {
  // canonicalMetadataForOntology labels the cache artifact's channel; it must
  // agree with the constants the per-ontology push helpers register on the
  // host, keyed by the same predicates that pick the push helper.
  using mosaico::canonicalMetadataForOntology;
  EXPECT_EQ(canonicalMetadataForOntology("image"), mosaico::kCanonicalImageMetadata);
  EXPECT_EQ(canonicalMetadataForOntology("compressed_image"), mosaico::kCanonicalImageMetadata);
  EXPECT_EQ(canonicalMetadataForOntology("pose"), mosaico::kCanonicalPosesInFrameMetadata);
  EXPECT_EQ(canonicalMetadataForOntology("frame_transform"), mosaico::kCanonicalFrameTransformsMetadata);
  EXPECT_EQ(canonicalMetadataForOntology("occupancy_grid"), mosaico::kCanonicalOccupancyGridMetadata);
  EXPECT_EQ(canonicalMetadataForOntology("grid_cells"), mosaico::kCanonicalSceneEntitiesMetadata);
  // Cloud-like tags (point cloud / laser scan / futures) share the record.
  EXPECT_EQ(canonicalMetadataForOntology("point_cloud"), mosaico::kCanonicalPointCloudMetadata);
  EXPECT_EQ(canonicalMetadataForOntology("laser_scan"), mosaico::kCanonicalPointCloudMetadata);
}
