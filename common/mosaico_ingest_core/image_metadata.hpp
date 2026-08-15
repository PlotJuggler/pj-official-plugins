// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The producer half of the image-topic metadata contract. This is the EXACT
// topic-level metadata_json the PJ4 consumer (CatalogModel +
// Media2DDockWidget) keys on to route a topic to the 2D view and select the
// canonical pj_base image decoder.
//
// Geometry is NO LONGER topic-level: every frame is serialized as one
// canonical PJ.Image blob (pj_base's PJ::serializeImage) that carries its own
// width/height/stride/encoding/is_bigendian. So the topic metadata only needs
// the type tag and the codec id — see pushImageRowsToHost in arrow_ingest.cpp.
//
//   "builtin_object_type": "kImage"        (routes the topic to the 2D view)
//   "image_codec":         "pj_image_v1"   (consumer decodes each blob with
//                                            PJ::deserializeImage)

#pragma once

#include <string_view>

namespace mosaico {

// The canonical, frozen image-topic metadata JSON. Keep byte-for-byte in sync
// with the PJ4 consumer's parser.
inline constexpr std::string_view kCanonicalImageMetadata =
    R"({"builtin_object_type":"kImage","image_codec":"pj_image_v1"})";

}  // namespace mosaico
