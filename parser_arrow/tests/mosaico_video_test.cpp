// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// A `compressed_image` topic whose user metadata declares `video_codec` is decoded as a VideoFrame instead of an Image.

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../common/builtin_object_compat.hpp"
#include "pj_base/builtin/video_frame.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_runtime_recorder.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_ARROW_PARSER_PLUGIN_PATH
#error "PJ_ARROW_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

using Type = PJ::sdk::BuiltinObjectType;

struct Fixture {
  PJ::MessageParserLibrary library;
  PJ::MessageParserHandle handle{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry;
  PJ::sdk::testing::ParserWriteRecorder write_host;
  PJ::sdk::testing::ParserRuntimeRecorder runtime_host;

  void setUp() {
    auto loaded = PJ::MessageParserLibrary::load(PJ_ARROW_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(loaded) << loaded.error();
    library = std::move(*loaded);
    handle = library.createHandle();
    ASSERT_TRUE(handle.valid());
    registry.registerService<PJ::sdk::ParserWriteHostService>(write_host.makeHost());
    registry.registerService<PJ::sdk::ParserRuntimeHostService>(runtime_host.makeHost());
    ASSERT_TRUE(handle.bind(registry.view()));
  }
};

struct VideoRow {
  std::string data = "frame";
  std::string format = "hevc";
  int64_t key_ns = 5000;
  bool with_header = true;
  int32_t seconds = 7;
  uint32_t nanoseconds = 123;
};

std::shared_ptr<arrow::Schema> makeSchema(bool with_header, const std::string& user_metadata) {
  arrow::FieldVector fields{arrow::field("timestamp_ns", arrow::int64())};
  if (with_header) {
    fields.push_back(
        arrow::field(
            "header", arrow::struct_(
                          {arrow::field(
                               "timestamp", arrow::struct_(
                                                {arrow::field("seconds", arrow::int32()),
                                                 arrow::field("nanoseconds", arrow::uint32())})),
                           arrow::field("frame_id", arrow::utf8())})));
  }
  fields.push_back(arrow::field("data", arrow::binary()));
  fields.push_back(arrow::field("format", arrow::utf8()));
  std::vector<std::string> keys{"mosaico:properties"};
  std::vector<std::string> values{R"({"ontology_tag":"compressed_image"})"};
  if (!user_metadata.empty()) {
    keys.emplace_back("mosaico:user_metadata");
    values.push_back(user_metadata);
  }
  return arrow::schema(fields, arrow::KeyValueMetadata::Make(keys, values));
}

std::vector<uint8_t> toBytes(const std::shared_ptr<arrow::Buffer>& buffer) {
  return {buffer->data(), buffer->data() + buffer->size()};
}

// One IPC stream, schema first; `row` absent gives the schema-only stream a connector hands to bindSchema.
std::vector<uint8_t> makeStream(const std::shared_ptr<arrow::Schema>& schema, const std::optional<VideoRow>& row) {
  auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
  auto writer = arrow::ipc::MakeStreamWriter(sink, schema).ValueOrDie();
  if (row) {
    arrow::Int64Builder key;
    arrow::BinaryBuilder data;
    arrow::StringBuilder format;
    EXPECT_TRUE(key.Append(row->key_ns).ok());
    EXPECT_TRUE(data.Append(row->data).ok());
    EXPECT_TRUE(format.Append(row->format).ok());
    std::vector<std::shared_ptr<arrow::Array>> columns{key.Finish().ValueOrDie()};
    if (schema->GetFieldByName("header")) {
      auto header_type = schema->GetFieldByName("header")->type();
      auto timestamp_type = header_type->field(0)->type();
      arrow::Int32Builder seconds;
      arrow::UInt32Builder nanoseconds;
      EXPECT_TRUE(seconds.Append(row->seconds).ok());
      EXPECT_TRUE(nanoseconds.Append(row->nanoseconds).ok());
      auto timestamp = arrow::StructArray::Make(
                           {seconds.Finish().ValueOrDie(), nanoseconds.Finish().ValueOrDie()}, timestamp_type->fields())
                           .ValueOrDie();
      arrow::StringBuilder frame_id;
      EXPECT_TRUE(frame_id.Append("camera_front").ok());
      columns.push_back(
          arrow::StructArray::Make({timestamp, frame_id.Finish().ValueOrDie()}, header_type->fields()).ValueOrDie());
    }
    columns.push_back(data.Finish().ValueOrDie());
    columns.push_back(format.Finish().ValueOrDie());
    EXPECT_TRUE(writer->WriteRecordBatch(*arrow::RecordBatch::Make(schema, 1, columns)).ok());
  }
  EXPECT_TRUE(writer->Close().ok());
  return toBytes(sink->Finish().ValueOrDie());
}

constexpr auto kHevc = R"({"video_codec":"hevc"})";

TEST(MosaicoVideo, DeclaresVideoFrameWhenTheTopicCarriesAVideoCodec) {
  Fixture fixture;
  fixture.setUp();
  const auto schema = makeStream(makeSchema(true, kHevc), std::nullopt);
  ASSERT_TRUE(fixture.handle.bindSchema("compressed_image", {schema.data(), schema.size()}));
  EXPECT_EQ(fixture.handle.classifySchema("compressed_image", {schema.data(), schema.size()}), Type::kVideoFrame);
}

TEST(MosaicoVideo, KeepsTheImageRouteWithoutAVideoCodec) {
  for (const std::string user_metadata : {"", "{}", R"({"video_codec":"vp9"})", R"({"video_codec":3})", "not json"}) {
    Fixture fixture;
    fixture.setUp();
    const auto schema = makeStream(makeSchema(true, user_metadata), std::nullopt);
    ASSERT_TRUE(fixture.handle.bindSchema("compressed_image", {schema.data(), schema.size()})) << user_metadata;
    EXPECT_EQ(fixture.handle.classifySchema("compressed_image", {schema.data(), schema.size()}), Type::kImage)
        << user_metadata;
  }
}

TEST(MosaicoVideo, KeepsTheImageRouteWhenTheSchemaIsUnreadable) {
  Fixture fixture;
  fixture.setUp();
  ASSERT_TRUE(fixture.handle.bindSchema("compressed_image", {}));
  EXPECT_EQ(fixture.handle.classifySchema("compressed_image", {}), Type::kImage);
}

TEST(MosaicoVideo, OnlyCompressedImageTopicsCanBeVideo) {
  Fixture fixture;
  fixture.setUp();
  const auto schema = makeStream(makeSchema(true, kHevc), std::nullopt);
  ASSERT_TRUE(fixture.handle.bindSchema("image", {schema.data(), schema.size()}));
  EXPECT_EQ(fixture.handle.classifySchema("image", {schema.data(), schema.size()}), Type::kImage);
}

PJ::Expected<PJ::sdk::ObjectRecord> decode(
    Fixture& fixture, const std::shared_ptr<arrow::Schema>& schema, const VideoRow& row) {
  const auto bind = makeStream(schema, std::nullopt);
  EXPECT_TRUE(fixture.handle.bindSchema("compressed_image", {bind.data(), bind.size()}));
  EXPECT_EQ(fixture.handle.classifySchema("compressed_image", {bind.data(), bind.size()}), Type::kVideoFrame);
  auto message = makeStream(schema, row);
  return fixture.handle.parseObjectFunctional(999, PJ::sdk::makePayloadView(std::move(message)));
}

TEST(MosaicoVideo, KeysFramesByMessageTimeAndDisplaysThemAtHeaderTime) {
  Fixture fixture;
  fixture.setUp();
  const auto object = decode(fixture, makeSchema(true, kHevc), VideoRow{});
  ASSERT_TRUE(object) << object.error();
  EXPECT_EQ(object->ts, 5000);
  const auto* frame = pj_compat::getBuiltinObject<PJ::sdk::VideoFrame>(object->object);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->format, "h265");
  EXPECT_EQ(frame->timestamp_ns, 7'000'000'123);
  EXPECT_EQ(frame->frame_id, "camera_front");
  EXPECT_EQ(std::string(frame->data.begin(), frame->data.end()), "frame");
}

TEST(MosaicoVideo, MapsH264AndKeepsThePayloadAnchored) {
  Fixture fixture;
  fixture.setUp();
  const auto object = decode(fixture, makeSchema(true, R"({"video_codec":"h264"})"), VideoRow{.format = "h264"});
  ASSERT_TRUE(object) << object.error();
  const auto* frame = pj_compat::getBuiltinObject<PJ::sdk::VideoFrame>(object->object);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->format, "h264");
  EXPECT_NE(frame->anchor, nullptr);
}

TEST(MosaicoVideo, AcceptsH265AsAnAliasOfHevc) {
  Fixture fixture;
  fixture.setUp();
  const auto object = decode(fixture, makeSchema(true, R"({"video_codec":"h265"})"), VideoRow{.format = "h265"});
  ASSERT_TRUE(object) << object.error();
  const auto* frame = pj_compat::getBuiltinObject<PJ::sdk::VideoFrame>(object->object);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->format, "h265");
}

TEST(MosaicoVideo, FallsBackToTheKeyWithoutAHeader) {
  Fixture fixture;
  fixture.setUp();
  const auto object = decode(fixture, makeSchema(false, kHevc), VideoRow{.with_header = false});
  ASSERT_TRUE(object) << object.error();
  const auto* frame = pj_compat::getBuiltinObject<PJ::sdk::VideoFrame>(object->object);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->timestamp_ns, 5000);
  EXPECT_TRUE(frame->frame_id.empty());
}

TEST(MosaicoVideo, RejectsAFrameWithAnUnknownCodecOrNoData) {
  Fixture fixture;
  fixture.setUp();
  const auto unknown = decode(fixture, makeSchema(true, kHevc), VideoRow{.format = "jpeg"});
  ASSERT_FALSE(unknown);
  EXPECT_NE(unknown.error().find("unsupported codec 'jpeg'"), std::string::npos);
  const auto empty = decode(fixture, makeSchema(true, kHevc), VideoRow{.data = ""});
  ASSERT_FALSE(empty);
  EXPECT_NE(empty.error().find("empty 'data'"), std::string::npos);
}

}  // namespace
