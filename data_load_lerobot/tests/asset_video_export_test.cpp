// Round-trip tests for the sdk::AssetVideo entries the LeRobot plugin pushes
// to ObjectStore. We exercise the serializeAssetVideo / deserializeAssetVideo
// pair directly — the plugin's importVideoAssets() just packages a struct
// with the same fields and lets the codec do the work, so a clean round-trip
// here is the load-bearing guarantee that pj_app's Media2DDockWidget will
// read back exactly what we pushed.

#include <gtest/gtest.h>

#include <pj_base/builtin/asset_video.hpp>
#include <pj_base/builtin/asset_video_codec.hpp>

TEST(AssetVideoExport, SerializeRoundTripsBasicFields) {
  PJ::sdk::AssetVideo in;
  in.file_path = "/datasets/pusht/videos/chunk-000/observation.image/episode_000000.mp4";
  in.time_origin_ns = PJ::Timestamp{0};
  in.frame_rate = 10.0;

  const auto bytes = PJ::serializeAssetVideo(in);
  const auto out = PJ::deserializeAssetVideo(bytes.data(), bytes.size());
  ASSERT_TRUE(out.has_value());

  EXPECT_EQ(out->file_path, in.file_path);
  ASSERT_TRUE(out->time_origin_ns.has_value());
  EXPECT_EQ(*out->time_origin_ns, 0);
  EXPECT_DOUBLE_EQ(out->frame_rate, 10.0);
}

TEST(AssetVideoExport, EmptyOptionalsRoundTripAsAbsent) {
  // Defaults: width/height/start_ns/end_ns/media_type/time_origin_ns unset
  // means the producer wants PJ4 to probe the file and play the whole video.
  // The wire format must preserve "absent" as "absent", not collapse it to a
  // sentinel value.
  PJ::sdk::AssetVideo in;
  in.file_path = "/x/y.mp4";

  const auto bytes = PJ::serializeAssetVideo(in);
  const auto out = PJ::deserializeAssetVideo(bytes.data(), bytes.size());
  ASSERT_TRUE(out.has_value());

  EXPECT_EQ(out->file_path, "/x/y.mp4");
  EXPECT_FALSE(out->time_origin_ns.has_value());
  EXPECT_FALSE(out->start_ns.has_value());
  EXPECT_FALSE(out->end_ns.has_value());
  EXPECT_EQ(out->width, 0u);
  EXPECT_EQ(out->height, 0u);
  EXPECT_DOUBLE_EQ(out->frame_rate, 0.0);
  EXPECT_TRUE(out->media_type.empty());
}
