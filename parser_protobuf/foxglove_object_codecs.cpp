// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include "foxglove_object_codecs.hpp"

#include <google/protobuf/io/coded_stream.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace pj_protobuf {
namespace {

namespace gpio = google::protobuf::io;
using gpio::CodedInputStream;

constexpr uint32_t kWireVarint = 0;
constexpr uint32_t kWireI64 = 1;
constexpr uint32_t kWireLen = 2;
constexpr uint32_t kWireI32 = 5;

[[nodiscard]] int fieldOf(uint32_t tag) {
  return static_cast<int>(tag >> 3);
}
[[nodiscard]] uint32_t wireOf(uint32_t tag) {
  return tag & 0x7u;
}

bool skipField(CodedInputStream& in, uint32_t wire_type) {
  switch (wire_type) {
    case kWireVarint: {
      uint64_t v = 0;
      return in.ReadVarint64(&v);
    }
    case kWireI64: {
      uint64_t v = 0;
      return in.ReadLittleEndian64(&v);
    }
    case kWireLen: {
      uint32_t len = 0;
      return in.ReadVarint32(&len) && in.Skip(static_cast<int>(len));
    }
    case kWireI32: {
      uint32_t v = 0;
      return in.ReadLittleEndian32(&v);
    }
    default:
      return false;
  }
}

bool readDouble(CodedInputStream& in, double& out) {
  uint64_t bits = 0;
  if (!in.ReadLittleEndian64(&bits)) {
    return false;
  }
  out = std::bit_cast<double>(bits);
  return true;
}

bool readString(CodedInputStream& in, std::string& out) {
  uint32_t len = 0;
  return in.ReadVarint32(&len) && in.ReadString(&out, static_cast<int>(len));
}

// A length-delimited `bytes` field straight into a uint8 vector. ReadRaw copies
// once into the resized buffer (vs the double copy of reading to a std::string
// then assigning) — matters for large inline payloads like an embedded glTF.
bool readBytes(CodedInputStream& in, std::vector<uint8_t>& out) {
  uint32_t len = 0;
  if (!in.ReadVarint32(&len)) {
    return false;
  }
  out.resize(len);
  return len == 0 || in.ReadRaw(out.data(), static_cast<int>(len));
}

/// foxglove Color (double r/g/b/a in [0,1]) -> sdk::ColorRGBA (uint8 0..255).
[[nodiscard]] uint8_t toU8(double c) {
  return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
}

// RAII for a CodedInputStream length limit over one nested submessage.
// The destructor skips any bytes the reader left unconsumed (e.g. after
// `break`ing on a malformed field) BEFORE popping the limit: PopLimit only
// restores the parent's byte ceiling, it does NOT advance the cursor, so
// without the skip a partial nested read would strand the parent mid-submessage
// and misread interior bytes as the next sibling's tag. Skipping to the limit
// makes the parent always resume exactly at the submessage boundary.
struct SubMessage {
  CodedInputStream& in;
  CodedInputStream::Limit limit;
  SubMessage(CodedInputStream& s, uint32_t len) : in(s), limit(s.PushLimit(static_cast<int>(len))) {}
  ~SubMessage() {
    if (const int remaining = in.BytesUntilLimit(); remaining > 0) {
      in.Skip(remaining);
    }
    in.PopLimit(limit);
  }
};

// ---------------------------------------------------------------------------
// Decode-robustness contract (deliberate, not accidental):
//   * The nested readers below (geometry + scene/annotation primitives) are
//     best-effort / LENIENT: each returns a plain value (not Expected) and
//     `break`s out of its field loop on a malformed/truncated field, returning
//     whatever was parsed so far. Its `SubMessage` skips any unconsumed bytes
//     and PopLimits in the destructor, so a partial nested read resumes the
//     parent exactly at the submessage boundary and never desyncs it.
//   * The top-level deserialize* entry points are STRICT: they return
//     PJ::unexpected on a malformed top-level tag or a failed skipField.
// Rationale: for a visualization consumer, rendering a partially-decoded frame
// (e.g. 99 of 100 annotations) beats dropping the whole message on one bad byte.
// This only affects corrupt/truncated payloads; well-formed Foxglove messages
// always decode in full. Consumers must treat primitive fields (counts,
// indices) as untrusted and bounds-check against the sibling arrays.
// ---------------------------------------------------------------------------

// --- nested geometry readers (each consumes exactly `len` bytes) ---

PJ::sdk::Vector3 readVector3(CodedInputStream& in, uint32_t len) {
  PJ::sdk::Vector3 v;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    if (wireOf(tag) == kWireI64 && f >= 1 && f <= 3) {
      double d = 0;
      if (!readDouble(in, d)) {
        break;
      }
      (f == 1 ? v.x : f == 2 ? v.y : v.z) = d;
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return v;
}

PJ::sdk::Quaternion readQuaternion(CodedInputStream& in, uint32_t len) {
  // proto3 omits default-valued (0.0) fields, so the wire is authoritative for
  // a PRESENT orientation submessage: start from all-zero, not the SDK's
  // identity default (w=1.0). Otherwise a 180° rotation about an axis — which
  // legitimately has w=0 and therefore omits w on the wire — would decode as
  // {x,y,z,1} and silently corrupt the rotation. (A fully-absent orientation
  // field keeps readPose's identity default, since readQuaternion isn't called.)
  PJ::sdk::Quaternion q;
  q.w = 0.0;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    if (wireOf(tag) == kWireI64 && f >= 1 && f <= 4) {
      double d = 0;
      if (!readDouble(in, d)) {
        break;
      }
      (f == 1 ? q.x : f == 2 ? q.y : f == 3 ? q.z : q.w) = d;
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return q;
}

PJ::sdk::ColorRGBA readColor(CodedInputStream& in, uint32_t len) {
  double r = 0, g = 0, b = 0, a = 0;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    if (wireOf(tag) == kWireI64 && f >= 1 && f <= 4) {
      double d = 0;
      if (!readDouble(in, d)) {
        break;
      }
      (f == 1 ? r : f == 2 ? g : f == 3 ? b : a) = d;
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return PJ::sdk::ColorRGBA{toU8(r), toU8(g), toU8(b), toU8(a)};
}

PJ::sdk::Pose readPose(CodedInputStream& in, uint32_t len) {
  PJ::sdk::Pose pose;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sub_len = 0;
    if (wireOf(tag) == kWireLen && f == 1 && in.ReadVarint32(&sub_len)) {
      pose.position = readVector3(in, sub_len);
    } else if (wireOf(tag) == kWireLen && f == 2 && in.ReadVarint32(&sub_len)) {
      pose.orientation = readQuaternion(in, sub_len);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return pose;
}

template <typename Point>
Point readPointXY(CodedInputStream& in, uint32_t len) {  // Point2: x=1,y=2
  Point p;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    if (wireOf(tag) == kWireI64 && f >= 1 && f <= 2) {
      double d = 0;
      if (!readDouble(in, d)) {
        break;
      }
      (f == 1 ? p.x : p.y) = d;
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return p;
}

PJ::sdk::Point3 readPoint3(CodedInputStream& in, uint32_t len) {  // x=1,y=2,z=3
  PJ::sdk::Point3 p;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    if (wireOf(tag) == kWireI64 && f >= 1 && f <= 3) {
      double d = 0;
      if (!readDouble(in, d)) {
        break;
      }
      (f == 1 ? p.x : f == 2 ? p.y : p.z) = d;
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return p;
}

int64_t readTimestampNs(CodedInputStream& in, uint32_t len) {  // seconds=1, nanos=2
  int64_t seconds = 0, nanos = 0;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    if (wireOf(tag) == kWireVarint && (f == 1 || f == 2)) {
      uint64_t v = 0;
      if (!in.ReadVarint64(&v)) {
        break;
      }
      if (f == 1) {
        seconds = static_cast<int64_t>(v);
      } else {
        nanos = static_cast<int64_t>(static_cast<int32_t>(v));
      }
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return seconds * 1'000'000'000LL + nanos;
}

// Read a packed `repeated double` block of `len` bytes into `out`.
void readPackedDoubles(CodedInputStream& in, uint32_t len, std::vector<double>& out) {
  SubMessage sub(in, len);
  double d = 0;
  while (readDouble(in, d)) {
    out.push_back(d);
  }
}

// Read a packed `repeated fixed32` block into `out`.
void readPackedFixed32(CodedInputStream& in, uint32_t len, std::vector<uint32_t>& out) {
  SubMessage sub(in, len);
  uint32_t v = 0;
  while (in.ReadLittleEndian32(&v)) {
    out.push_back(v);
  }
}

}  // namespace

// ===========================================================================
// foxglove.FrameTransform -> sdk::FrameTransforms
// { timestamp=1, parent_frame_id=2, child_frame_id=3, translation=4 Vector3,
//   rotation=5 Quaternion }
// ===========================================================================
PJ::Expected<PJ::sdk::FrameTransforms> deserializeFoxgloveFrameTransform(const uint8_t* data, size_t size) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.FrameTransform: too large"));
  }
  CodedInputStream in(data, static_cast<int>(size));
  PJ::sdk::FrameTransform tf;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t len = 0;
    switch (f) {
      case 1:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.FrameTransform: bad timestamp"));
        }
        tf.timestamp = readTimestampNs(in, len);
        break;
      case 2:
        if (wireOf(tag) != kWireLen || !readString(in, tf.parent_frame_id)) {
          return PJ::unexpected(std::string("foxglove.FrameTransform: bad parent_frame_id"));
        }
        break;
      case 3:
        if (wireOf(tag) != kWireLen || !readString(in, tf.child_frame_id)) {
          return PJ::unexpected(std::string("foxglove.FrameTransform: bad child_frame_id"));
        }
        break;
      case 4:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.FrameTransform: bad translation"));
        }
        tf.translation = readVector3(in, len);
        break;
      case 5:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.FrameTransform: bad rotation"));
        }
        tf.rotation = readQuaternion(in, len);
        break;
      default:
        if (!skipField(in, wireOf(tag))) {
          return PJ::unexpected(std::string("foxglove.FrameTransform: malformed"));
        }
    }
  }
  PJ::sdk::FrameTransforms out;
  out.transforms.push_back(std::move(tf));
  return out;
}

// ===========================================================================
// foxglove.CompressedImage -> sdk::Image  (zero-copy data)
// { timestamp=1, data=2 bytes, format=3 string, frame_id=4 string }
// ===========================================================================
PJ::Expected<PJ::sdk::Image> deserializeFoxgloveCompressedImageView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.CompressedImage: too large"));
  }
  CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());
  PJ::sdk::Image img;
  PJ::Span<const uint8_t> data_span;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t len = 0;
    switch (f) {
      case 1:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CompressedImage: bad timestamp"));
        }
        img.timestamp_ns = readTimestampNs(in, len);
        break;
      case 2: {  // data (bytes) — zero-copy
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CompressedImage: bad data"));
        }
        if (len > 0) {
          const void* ptr = nullptr;
          int avail = 0;
          if (!in.GetDirectBufferPointer(&ptr, &avail) || avail < static_cast<int>(len)) {
            return PJ::unexpected(std::string("foxglove.CompressedImage: data not contiguous"));
          }
          data_span = PJ::Span<const uint8_t>(static_cast<const uint8_t*>(ptr), len);
          if (!in.Skip(static_cast<int>(len))) {
            return PJ::unexpected(std::string("foxglove.CompressedImage: failed to skip data"));
          }
        }
        break;
      }
      case 3:
        if (wireOf(tag) != kWireLen || !readString(in, img.encoding)) {  // foxglove `format` -> sdk `encoding`
          return PJ::unexpected(std::string("foxglove.CompressedImage: bad format"));
        }
        break;
      case 4:  // frame_id -> sdk::Image.frame_id (lets the consumer match CameraInfo / place in 3D).
        if (wireOf(tag) != kWireLen || !readString(in, img.frame_id)) {
          return PJ::unexpected(std::string("foxglove.CompressedImage: bad frame_id"));
        }
        break;
      default:
        if (!skipField(in, wireOf(tag))) {
          return PJ::unexpected(std::string("foxglove.CompressedImage: malformed"));
        }
    }
  }
  // Compressed payload: width/height/row_step unknown (the decoder reads them
  // from the bitstream); is_bigendian irrelevant.
  img.data = data_span;
  img.anchor = std::move(anchor);
  return img;
}

// ===========================================================================
// foxglove.RawImage -> sdk::Image  (zero-copy data, UNCOMPRESSED pixels)
// { timestamp=1, frame_id=2 string, width=3 fixed32, height=4 fixed32,
//   encoding=5 string, step=6 fixed32, data=7 bytes }
// Unlike CompressedImage, the pixels are raw, so width/height/encoding/row_step
// MUST be carried through for the consumer to interpret `data` (same contract as
// a ROS sensor_msgs/Image — the encoding string drives the renderer).
// ===========================================================================
PJ::Expected<PJ::sdk::Image> deserializeFoxgloveRawImageView(
    const uint8_t* data, size_t size, PJ::sdk::BufferAnchor anchor) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.RawImage: too large"));
  }
  CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());
  PJ::sdk::Image img;
  PJ::Span<const uint8_t> data_span;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t len = 0;
    switch (f) {
      case 1:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad timestamp"));
        }
        img.timestamp_ns = readTimestampNs(in, len);
        break;
      case 2:
        if (wireOf(tag) != kWireLen || !readString(in, img.frame_id)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad frame_id"));
        }
        break;
      case 3: {
        uint32_t width = 0;
        if (wireOf(tag) != kWireI32 || !in.ReadLittleEndian32(&width)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad width"));
        }
        img.width = width;
        break;
      }
      case 4: {
        uint32_t height = 0;
        if (wireOf(tag) != kWireI32 || !in.ReadLittleEndian32(&height)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad height"));
        }
        img.height = height;
        break;
      }
      case 5:
        if (wireOf(tag) != kWireLen || !readString(in, img.encoding)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad encoding"));
        }
        break;
      case 6: {
        uint32_t step = 0;
        if (wireOf(tag) != kWireI32 || !in.ReadLittleEndian32(&step)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad step"));
        }
        img.row_step = step;
        break;
      }
      case 7: {  // data (bytes) — zero-copy
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.RawImage: bad data"));
        }
        if (len > 0) {
          const void* ptr = nullptr;
          int avail = 0;
          if (!in.GetDirectBufferPointer(&ptr, &avail) || avail < static_cast<int>(len)) {
            return PJ::unexpected(std::string("foxglove.RawImage: data not contiguous"));
          }
          data_span = PJ::Span<const uint8_t>(static_cast<const uint8_t*>(ptr), len);
          if (!in.Skip(static_cast<int>(len))) {
            return PJ::unexpected(std::string("foxglove.RawImage: failed to skip data"));
          }
        }
        break;
      }
      default:
        if (!skipField(in, wireOf(tag))) {
          return PJ::unexpected(std::string("foxglove.RawImage: malformed"));
        }
    }
  }
  img.data = data_span;
  img.anchor = std::move(anchor);
  return img;
}

// ===========================================================================
// foxglove.CameraCalibration -> sdk::CameraInfo
// { timestamp=1, width=2 fixed32, height=3 fixed32, distortion_model=4,
//   D=5 repeated double, K=6, R=7, P=8, frame_id=9 }
// ===========================================================================
PJ::Expected<PJ::sdk::CameraInfo> deserializeFoxgloveCameraCalibration(const uint8_t* data, size_t size) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.CameraCalibration: too large"));
  }
  CodedInputStream in(data, static_cast<int>(size));
  PJ::sdk::CameraInfo ci;
  std::vector<double> kmat, rmat, pmat;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t len = 0;
    switch (f) {
      case 1:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad timestamp"));
        }
        ci.timestamp_ns = readTimestampNs(in, len);
        break;
      case 2: {
        uint32_t w = 0;
        if (wireOf(tag) != kWireI32 || !in.ReadLittleEndian32(&w)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad width"));
        }
        ci.width = w;
        break;
      }
      case 3: {
        uint32_t h = 0;
        if (wireOf(tag) != kWireI32 || !in.ReadLittleEndian32(&h)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad height"));
        }
        ci.height = h;
        break;
      }
      case 4:
        if (wireOf(tag) != kWireLen || !readString(in, ci.distortion_model)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad distortion_model"));
        }
        break;
      case 5:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad D"));
        }
        readPackedDoubles(in, len, ci.D);
        break;
      case 6:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad K"));
        }
        readPackedDoubles(in, len, kmat);
        break;
      case 7:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad R"));
        }
        readPackedDoubles(in, len, rmat);
        break;
      case 8:
        if (wireOf(tag) != kWireLen || !in.ReadVarint32(&len)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad P"));
        }
        readPackedDoubles(in, len, pmat);
        break;
      case 9:
        if (wireOf(tag) != kWireLen || !readString(in, ci.frame_id)) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: bad frame_id"));
        }
        break;
      default:
        if (!skipField(in, wireOf(tag))) {
          return PJ::unexpected(std::string("foxglove.CameraCalibration: malformed"));
        }
    }
  }
  for (size_t i = 0; i < kmat.size() && i < ci.K.size(); ++i) {
    ci.K[i] = kmat[i];
  }
  for (size_t i = 0; i < rmat.size() && i < ci.R.size(); ++i) {
    ci.R[i] = rmat[i];
  }
  for (size_t i = 0; i < pmat.size() && i < ci.P.size(); ++i) {
    ci.P[i] = pmat[i];
  }
  return ci;
}

// ===========================================================================
// foxglove.ImageAnnotations -> sdk::ImageAnnotations
// { circles=1, points=2 (PointsAnnotation), texts=3 }
// ===========================================================================
namespace {

PJ::sdk::CircleAnnotation readCircle(CodedInputStream& in, uint32_t len, int64_t& timestamp_out) {
  // { timestamp=1, position=2 Point2, diameter=3, thickness=4, fill_color=5, outline_color=6 }
  PJ::sdk::CircleAnnotation c;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      timestamp_out = readTimestampNs(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      c.center = readPointXY<PJ::sdk::Point2>(in, sl);
    } else if (f == 3 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      c.radius = d / 2.0;  // foxglove diameter -> sdk radius
    } else if (f == 4 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      c.thickness = d;
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      c.fill_color = readColor(in, sl);
    } else if (f == 6 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      c.color = readColor(in, sl);  // outline_color -> color
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return c;
}

PJ::sdk::PointsAnnotation readPointsAnnotation(CodedInputStream& in, uint32_t len, int64_t& timestamp_out) {
  // { timestamp=1, type=2 enum, points=3 Point2[], outline_color=4, outline_colors=5, fill_color=6, thickness=7 }
  // foxglove type: UNKNOWN=0, POINTS=1, LINE_LOOP=2, LINE_STRIP=3, LINE_LIST=4
  PJ::sdk::PointsAnnotation pa;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      timestamp_out = readTimestampNs(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireVarint) {
      uint64_t t = 0;
      in.ReadVarint64(&t);
      using Topo = PJ::sdk::AnnotationTopology;
      pa.topology = t == 2   ? Topo::kLineLoop
                    : t == 3 ? Topo::kLineStrip
                    : t == 4 ? Topo::kLineList
                             : Topo::kPoints;  // 0/1 -> points
    } else if (f == 3 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      pa.points.push_back(readPointXY<PJ::sdk::Point2>(in, sl));
    } else if (f == 4 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      pa.color = readColor(in, sl);  // outline_color -> color
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      pa.colors.push_back(readColor(in, sl));
    } else if (f == 6 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      pa.fill_color = readColor(in, sl);
    } else if (f == 7 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      pa.thickness = d;
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return pa;
}

PJ::sdk::TextAnnotation readTextAnnotation(CodedInputStream& in, uint32_t len, int64_t& timestamp_out) {
  // { timestamp=1, position=2 Point2, text=3, font_size=4, text_color=5, background_color=6 }
  PJ::sdk::TextAnnotation t;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      timestamp_out = readTimestampNs(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.position = readPointXY<PJ::sdk::Point2>(in, sl);
    } else if (f == 3 && wireOf(tag) == kWireLen) {
      readString(in, t.text);
    } else if (f == 4 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      t.font_size = d;
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.color = readColor(in, sl);  // text_color -> color (background_color dropped)
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return t;
}

}  // namespace

PJ::Expected<PJ::sdk::ImageAnnotations> deserializeFoxgloveImageAnnotations(const uint8_t* data, size_t size) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.ImageAnnotations: too large"));
  }
  CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());
  PJ::sdk::ImageAnnotations ann;
  // foxglove.ImageAnnotations has no top-level timestamp; each sub-annotation
  // carries its own (field 1). Adopt the first non-zero one as the message
  // timestamp so the consumer can time-align the overlay to its image frame.
  int64_t first_timestamp_ns = 0;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t len = 0;
    int64_t ts = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&len)) {
      ann.circles.push_back(readCircle(in, len, ts));
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&len)) {
      ann.points.push_back(readPointsAnnotation(in, len, ts));
    } else if (f == 3 && wireOf(tag) == kWireLen && in.ReadVarint32(&len)) {
      ann.texts.push_back(readTextAnnotation(in, len, ts));
    } else if (!skipField(in, wireOf(tag))) {
      return PJ::unexpected(std::string("foxglove.ImageAnnotations: malformed"));
    }
    if (first_timestamp_ns == 0 && ts != 0) {
      first_timestamp_ns = ts;
    }
  }
  ann.timestamp = first_timestamp_ns;
  return ann;
}

// ===========================================================================
// foxglove.SceneUpdate -> sdk::SceneEntities
// { deletions=1, entities=2 (SceneEntity) }
// ===========================================================================
namespace {

PJ::sdk::ArrowPrimitive readArrow(CodedInputStream& in, uint32_t len) {
  // { pose=1, shaft_length=2, shaft_diameter=3, head_length=4, head_diameter=5, color=6 }
  PJ::sdk::ArrowPrimitive a;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      a.pose = readPose(in, sl);
    } else if (f >= 2 && f <= 5 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      (f == 2 ? a.shaft_length : f == 3 ? a.shaft_diameter : f == 4 ? a.head_length : a.head_diameter) = d;
    } else if (f == 6 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      a.color = readColor(in, sl);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return a;
}

// Cube/Sphere share the layout { pose=1, size=2 Vector3, color=3 }.
template <typename Prim>
Prim readBoxLike(CodedInputStream& in, uint32_t len) {
  Prim p;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      p.pose = readPose(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      p.size = readVector3(in, sl);
    } else if (f == 3 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      p.color = readColor(in, sl);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return p;
}

PJ::sdk::CylinderPrimitive readCylinder(CodedInputStream& in, uint32_t len) {
  // { pose=1, size=2, bottom_scale=3, top_scale=4, color=5 }
  // Decode faithfully to the foxglove wire: proto3 omits a 0.0 scale, and
  // foxglove's renderer reads an omitted scale as 0 (a face collapsed to a
  // point), not 1. Override the SDK's ergonomic 1.0 default so an omitted
  // scale stays 0 rather than silently becoming a full-diameter face.
  PJ::sdk::CylinderPrimitive c;
  c.bottom_scale = 0.0;
  c.top_scale = 0.0;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      c.pose = readPose(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      c.size = readVector3(in, sl);
    } else if (f == 3 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      c.bottom_scale = d;
    } else if (f == 4 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      c.top_scale = d;
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      c.color = readColor(in, sl);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return c;
}

PJ::sdk::LinePrimitive readLine(CodedInputStream& in, uint32_t len) {
  // { type=1 enum, pose=2, thickness=3, scale_invariant=4, points=5, color=6, colors=7, indices=8 fixed32[] }
  PJ::sdk::LinePrimitive l;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireVarint) {
      uint64_t t = 0;
      in.ReadVarint64(&t);
      l.type = static_cast<PJ::sdk::LineType>(t);  // values match (0/1/2)
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      l.pose = readPose(in, sl);
    } else if (f == 3 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      l.thickness = d;
    } else if (f == 4 && wireOf(tag) == kWireVarint) {
      uint64_t v = 0;
      in.ReadVarint64(&v);
      l.scale_invariant = (v != 0);
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      l.points.push_back(readPoint3(in, sl));
    } else if (f == 6 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      l.color = readColor(in, sl);
    } else if (f == 7 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      l.colors.push_back(readColor(in, sl));
    } else if (f == 8 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      readPackedFixed32(in, sl, l.indices);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return l;
}

PJ::sdk::TrianglePrimitive readTriangle(CodedInputStream& in, uint32_t len) {
  // { pose=1, points=2, color=3, colors=4, indices=5 fixed32[] }
  PJ::sdk::TrianglePrimitive t;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.pose = readPose(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.points.push_back(readPoint3(in, sl));
    } else if (f == 3 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.color = readColor(in, sl);
    } else if (f == 4 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.colors.push_back(readColor(in, sl));
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      readPackedFixed32(in, sl, t.indices);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return t;
}

PJ::sdk::TextPrimitive readText(CodedInputStream& in, uint32_t len) {
  // { pose=1, billboard=2, font_size=3, scale_invariant=4, color=5, text=6 }
  PJ::sdk::TextPrimitive t;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.pose = readPose(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireVarint) {
      uint64_t v = 0;
      in.ReadVarint64(&v);
      t.billboard = (v != 0);
    } else if (f == 3 && wireOf(tag) == kWireI64) {
      double d = 0;
      readDouble(in, d);
      t.font_size = d;
    } else if (f == 4 && wireOf(tag) == kWireVarint) {
      uint64_t v = 0;
      in.ReadVarint64(&v);
      t.scale_invariant = (v != 0);
    } else if (f == 5 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      t.color = readColor(in, sl);
    } else if (f == 6 && wireOf(tag) == kWireLen) {
      readString(in, t.text);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return t;
}

// foxglove.ModelPrimitive: a mesh asset, sourced either from `url` (a resolvable
// resource) or inline `data` tagged by `media_type`. Same lenient pattern as
// readBoxLike. `data` flows whole (no array clamp) so a downstream mesh loader
// gets the complete buffer rather than a truncated, silently-broken one.
PJ::sdk::ModelPrimitive readModel(CodedInputStream& in, uint32_t len) {
  // { pose=1, scale=2, color=3, override_color=4, url=5, media_type=6, data=7 (bytes) }
  PJ::sdk::ModelPrimitive m;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      m.pose = readPose(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      m.scale = readVector3(in, sl);
    } else if (f == 3 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      m.color = readColor(in, sl);
    } else if (f == 4 && wireOf(tag) == kWireVarint) {
      uint64_t v = 0;
      in.ReadVarint64(&v);
      m.override_color = (v != 0);
    } else if (f == 5 && wireOf(tag) == kWireLen) {
      readString(in, m.url);
    } else if (f == 6 && wireOf(tag) == kWireLen) {
      readString(in, m.media_type);
    } else if (f == 7 && wireOf(tag) == kWireLen) {
      readBytes(in, m.data);
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return m;
}

PJ::sdk::SceneEntity readSceneEntity(CodedInputStream& in, uint32_t len) {
  // { timestamp=1, frame_id=2, id=3, lifetime=4 Duration, frame_locked=5, metadata=6,
  //   arrows=7, cubes=8, spheres=9, cylinders=10, lines=11, triangles=12, texts=13, models=14 }
  PJ::sdk::SceneEntity e;
  SubMessage sub(in, len);
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t sl = 0;
    // metadata(6), unknowns and any wrong-wire-type field fall through
    // to the trailing skipField; a failed skip leaves the stream desynced, so stop.
    if (f == 1 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.timestamp = readTimestampNs(in, sl);
    } else if (f == 2 && wireOf(tag) == kWireLen) {
      readString(in, e.frame_id);
    } else if (f == 3 && wireOf(tag) == kWireLen) {
      readString(in, e.id);
    } else if (f == 4 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.lifetime_ns = readTimestampNs(in, sl);  // Duration has the same {sec, nanos} layout
    } else if (f == 5 && wireOf(tag) == kWireVarint) {
      uint64_t v = 0;
      in.ReadVarint64(&v);
      e.frame_locked = (v != 0);
    } else if (f == 7 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.arrows.push_back(readArrow(in, sl));
    } else if (f == 8 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.cubes.push_back(readBoxLike<PJ::sdk::CubePrimitive>(in, sl));
    } else if (f == 9 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.spheres.push_back(readBoxLike<PJ::sdk::SpherePrimitive>(in, sl));
    } else if (f == 10 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.cylinders.push_back(readCylinder(in, sl));
    } else if (f == 11 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.lines.push_back(readLine(in, sl));
    } else if (f == 12 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.triangles.push_back(readTriangle(in, sl));
    } else if (f == 13 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.texts.push_back(readText(in, sl));
    } else if (f == 14 && wireOf(tag) == kWireLen && in.ReadVarint32(&sl)) {
      e.models.push_back(readModel(in, sl));
    } else if (!skipField(in, wireOf(tag))) {
      break;
    }
  }
  return e;
}

}  // namespace

PJ::Expected<PJ::sdk::SceneEntities> deserializeFoxgloveSceneUpdate(const uint8_t* data, size_t size) {
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return PJ::unexpected(std::string("foxglove.SceneUpdate: too large"));
  }
  CodedInputStream in(data, static_cast<int>(size));
  in.SetTotalBytesLimit(std::numeric_limits<int>::max());
  PJ::sdk::SceneEntities scene;
  uint32_t tag = 0;
  while ((tag = in.ReadTag()) != 0) {
    const int f = fieldOf(tag);
    uint32_t len = 0;
    if (f == 2 && wireOf(tag) == kWireLen && in.ReadVarint32(&len)) {
      scene.entities.push_back(readSceneEntity(in, len));
    } else if (!skipField(in, wireOf(tag))) {  // deletions(1) skipped — nothing to render
      return PJ::unexpected(std::string("foxglove.SceneUpdate: malformed"));
    }
  }
  return scene;
}

}  // namespace pj_protobuf
