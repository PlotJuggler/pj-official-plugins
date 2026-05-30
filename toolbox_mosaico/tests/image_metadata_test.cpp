// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Pins the producer half of the image-topic metadata contract. The string
// asserted here is exactly what the PJ4 consumer (CatalogModel +
// Media2DDockWidget) parses, so this guards the cross-repo wire format from
// drifting on the mosaico side.
//
// Geometry is no longer topic-level — every frame is a self-describing
// canonical PJ.Image blob (see image_serialize_test.cpp). The topic metadata
// only carries the type tag + codec id.

#include "../src/image_metadata.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(ImageMetadataTest, CanonicalImageTopicMetadataIsFrozen) {
  EXPECT_EQ(
      std::string(mosaico::kCanonicalImageMetadata), R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})");
}

TEST(ImageMetadataTest, RoutesToTwoDViewAndSelectsCanonicalCodec) {
  const std::string meta(mosaico::kCanonicalImageMetadata);
  // The consumer keys on these exact tokens.
  EXPECT_NE(meta.find(R"("builtin_object_type":"kImage")"), std::string::npos);
  EXPECT_NE(meta.find(R"("image_codec":"pj_image_v1")"), std::string::npos);
  // Geometry is per-frame now — it must NOT appear at the topic level.
  EXPECT_EQ(meta.find("image_encoding"), std::string::npos);
  EXPECT_EQ(meta.find("image_width"), std::string::npos);
  EXPECT_EQ(meta.find("image_format"), std::string::npos);
}
