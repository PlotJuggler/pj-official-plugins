// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/c/abi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <string_view>

// Forward-declared so the header doesn't pull in <arrow/api.h>.
namespace arrow {
class Table;
template <typename T>
class Result;
}  // namespace arrow

namespace mosaico {

/// Auto-detect the timestamp column name in an Arrow Schema:
///   1. Any field whose type is TIMESTAMP.
///   2. Otherwise, the first name in {timestamp_ns, recording_timestamp_ns,
///      timestamp, time, ts}.
/// Returns the detected name or "" if none.
[[nodiscard]] std::string detectTimestampColumn(const ArrowSchema* schema);

/// Walk every column of @p table and flatten any STRUCT-typed columns into
/// individual primitive columns named `<parent>/<child>`. Mirrors PJ3's
/// `flattenArray` (toolbox_mosaico.cpp:341) — without it, ROS-shaped topics
/// like `nav_msgs/Odometry` (whose `pose` and `twist` are struct columns)
/// reach the host as opaque struct entries that PJ4's arrow_import silently
/// drops, so the dataset tree only shows the timestamp.
///
/// Non-struct columns pass through unchanged. List/map/union columns also
/// pass through — PJ3 only handles primitive + struct; matching that.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>> flattenStructColumns(std::shared_ptr<arrow::Table> table);

/// Cast Arrow "view" string/binary columns (Utf8View / BinaryView) to canonical
/// Utf8 / Binary. pj_datastore's nanoarrow import only maps STRING / LARGE_STRING
/// and a view-typed column corrupts the whole record-batch import (every column
/// reads null), so the scalar appendArrowStream pipeline normalizes them first.
/// Columns without a view type pass through unchanged.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>> normalizeViewColumns(std::shared_ptr<arrow::Table> table);

/// Pump an ArrowArrayStream into the toolbox host as a single topic under a
/// previously created data source.
///
/// `source` is the per-Download DataSourceHandle (one per sequence, shared by
/// every topic — see FetchWorker::datasetForFetch). `topic_name` is the BARE
/// topic name; it is NOT prefixed with the sequence, because the data source
/// already carries the sequence name. `timestamp_col` is the field to use as
/// the X axis; the caller should run detectTimestampColumn against the
/// stream's schema first.
///
/// Takes ownership of `stream` on success — the host's appendArrowStream
/// implementation calls stream->release. On failure the caller still owns
/// the stream and must release it.
[[nodiscard]] PJ::Status pumpStreamToHost(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::DataSourceHandle source, std::string_view topic_name,
    ArrowArrayStream* stream, std::string_view timestamp_col);

/// Outcome of a per-row canonical-object push (image / point cloud / transform /
/// pose). A non-fatal per-row skip (missing data, malformed geometry, …)
/// increments `skipped` and records the first reason in `first_error` without
/// aborting the topic — `pushed` is the count actually serialized and handed to
/// the host.
struct ObjectPushOutcome {
  std::int64_t pushed = 0;
  std::int64_t skipped = 0;
  std::string first_error;
};
/// Back-compat alias for the original image-only name.
using ImagePushOutcome = ObjectPushOutcome;

/// Shared write context for the per-row canonical-object push helpers
/// (pushImageRowsToHost / pushPointCloudRowsToHost / pushPoseRowsToHost). It
/// bundles the write target (host + per-Download data source + BARE topic name)
/// and the synthetic-timestamp policy used for rows that carry no explicit
/// timestamp: ts = synth_anchor_ns + row * synth_interval_ns. The three helpers
/// took this identical 6-tuple verbatim, so it travels as one value.
///
/// `host` is a trivially-copyable view (two ABI pointers) held by value;
/// `topic_name`/`ts_field` are owned so a context can be built from string
/// literals (tests) or borrowed lvalues alike.
struct ObjectIngestContext {
  PJ::sdk::ToolboxHostView host;
  PJ::sdk::DataSourceHandle source;
  std::string topic_name;  ///< BARE topic name; the data source carries the sequence.
  std::string ts_field;    ///< timestamp column to use, or "" to synthesize timestamps.
  std::int64_t synth_anchor_ns = 0;
  std::int64_t synth_interval_ns = 0;
  /// Optional callback invoked after every SUCCESSFUL host push with the exact
  /// (timestamp, blob bytes) pushed. Artifact capture uses it to make replay
  /// byte-identical. Must not throw; capture records its own failure and goes
  /// inert without failing the ingest.
  std::function<void(std::int64_t ts_ns, const std::uint8_t* data, std::size_t size)> on_blob_pushed;
};

/// The one callback every push helper makes after a successful host push,
/// centralized so the post-success, exact-bytes contract cannot drift across
/// the per-ontology helpers.
inline void notifyBlobPushed(const ObjectIngestContext& ctx, std::int64_t ts_ns, const std::vector<uint8_t>& blob) {
  if (ctx.on_blob_pushed) {
    ctx.on_blob_pushed(ts_ns, blob.data(), blob.size());
  }
}

/// Serialize every row of @p table as a canonical PJ.Image blob (pj_base's
/// PJ::serializeImage) and push it into the host's ObjectStore under
/// @p ctx.source, keyed by the BARE @p ctx.topic_name.
///
/// The object topic is registered ONCE with the canonical metadata JSON
///   {"builtin_object_type":"kImage","image_codec":"pj_image_v1"}
/// — geometry is per-frame inside each blob, not topic-level.
///
/// Per row it reads width/height/stride (int32), encoding+format (string;
/// STRING/LARGE_STRING/STRING_VIEW), is_bigendian (bool; null/absent -> host
/// native endianness, per the canonical Image model),
/// data (binary; BINARY/LARGE_BINARY/FIXED_SIZE_BINARY/BINARY_VIEW), and the
/// timestamp from @p ctx.ts_field if present (otherwise the synthetic policy in
/// @p ctx). `encoding` falls back to `format` when empty so pure-compressed
/// topics (only `format`) still carry an encoding. A row missing required
/// columns is skipped (see ImagePushOutcome) rather than aborting the topic.
///
/// Returns an error only on a fatal failure (host unbound, missing `data`
/// column entirely, or a host register/push rejection).
[[nodiscard]] PJ::Expected<ImagePushOutcome> pushImageRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of the Mosaico `point_cloud2` ontology table as one
/// canonical PJ.PointCloud blob (pj_base's PJ::serializePointCloud) and push it
/// into the host's ObjectStore under @p source, keyed by the BARE @p topic_name.
///
/// The object topic is registered ONCE with {"builtin_object_type":"kPointCloud"}.
/// `point_cloud2` is ROS PointCloud2-shaped — a packed `data` (binary) blob plus a
/// `fields` descriptor (list<struct{name,offset,datatype,count}>) and
/// width/height/point_step/row_step/is_bigendian/is_dense/frame_id — which IS the
/// canonical sdk::PointCloud layout, so this copies it through verbatim. It
/// performs NO decode / decompression (host-side coloring/conversion happens in
/// pj_scene3D). A row with empty `data` or `fields` is skipped (see
/// ObjectPushOutcome). Returns an error only on a fatal failure (host unbound,
/// `data`/`fields` columns absent entirely, or a host register/push rejection).
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushPointCloudRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of a Mosaico pose-like ontology as one canonical
/// PJ.PosesInFrame blob (one pose per row, in the row's `frame_id`) and push it
/// under {"builtin_object_type":"kPosesInFrame"}. Struct columns are flattened
/// first; it reads position/{x,y,z} + orientation/{x,y,z,w} for the `pose`
/// ontology, OR pose/position/* + pose/orientation/* for `motion_state`
/// (odometry, which nests the pose) — whichever prefix is present.
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushPoseRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of a Mosaico `transform` / `frame_transform` ontology as
/// one canonical PJ.FrameTransforms blob (pj_base's PJ::serializeFrameTransforms)
/// and push it under {"builtin_object_type":"kFrameTransforms"}.
///
/// Two input shapes are accepted (auto-detected per the table's columns):
///  - `transform`: one transform per row. Struct columns are flattened; it reads
///    translation/{x,y,z} + rotation/{x,y,z,w}, with parent_frame_id from
///    header/frame_id and child_frame_id from target_frame_id.
///  - `frame_transform`: a `transforms` list<struct<…>> column per row — each
///    list becomes one FrameTransforms batch (the list is read directly, not
///    flattened).
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushFrameTransformsRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of the Mosaico `occupancy_grid` ontology as one canonical
/// PJ.OccupancyGrid blob (pj_base's PJ::serializeOccupancyGrid) and push it under
/// {"builtin_object_type":"kOccupancyGrid"} — the same builtin the ROS/MCAP path
/// uses for nav_msgs/OccupancyGrid.
///
/// Struct columns are flattened; it reads info/{resolution,width,height} and the
/// origin pose (info/origin/position/* + info/origin/orientation/*), and the
/// dense `data` list<int8> column directly (values −1 unknown / 0 free / 1-100
/// occupied, row-major, length width*height). A row whose `data` is shorter than
/// width*height is skipped.
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushOccupancyGridRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of the Mosaico `laser_scan` ontology as one canonical
/// PJ.PointCloud blob (pj_base's PJ::serializePointCloud) and push it under
/// {"builtin_object_type":"kPointCloud"}.
///
/// The polar scan (angle_min/angle_increment + `ranges` list<float>) is expanded
/// into packed XYZ points: beam i sits at angle = angle_min + i*angle_increment,
/// (x,y,z) = (r·cos θ, r·sin θ, 0). A non-finite or out-of-[range_min,range_max]
/// return is dropped. `intensities` (when present) becomes an `intensity` field.
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushLaserScanRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of the Mosaico `grid_cells` ontology as one canonical
/// PJ.SceneEntities blob (pj_base's PJ::serializeSceneEntities) and push it under
/// {"builtin_object_type":"kSceneEntities"}: one flat CubePrimitive per marked
/// cell, centered at the cell's (x,y,z) with extents (cell_width, cell_height, ~0).
/// Reads cell_width/cell_height plus the `cells` list<struct{…,x,y,z}> column.
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushGridCellsRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

/// Serialize every row of a Mosaico experimental "futures" sensor-cloud ontology
/// (`lidar` / `radar` / `rgbd_camera` / `tof_camera` / `stereo_camera`) as one
/// canonical PJ.PointCloud blob and push it under
/// {"builtin_object_type":"kPointCloud"}.
///
/// Unlike `point_cloud2`, these arrive COLUMNAR: one Arrow list column per
/// per-point attribute. The converter reads the parallel attribute lists (x/y/z
/// required, every other recognized numeric list column optional), packs them
/// into an interleaved PointCloud `data` buffer with a synthesized `fields`
/// descriptor (offsets/point_step derived from each attribute's Arrow element
/// type), and serializes. Attribute columns of differing length are truncated to
/// the x-column point count; a row with no `x` column (or zero points) is skipped.
[[nodiscard]] PJ::Expected<ObjectPushOutcome> pushColumnarPointCloudRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table);

}  // namespace mosaico
