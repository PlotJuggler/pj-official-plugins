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

using mosaico::isImageOntology;
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

}  // namespace
