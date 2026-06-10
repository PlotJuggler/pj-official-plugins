# parser_ros: yolo_msgs/DetectionArray → ImageAnnotations

Design notes for the YOLO detection → `PJ::sdk::ImageAnnotations` conversion
(`ros_yolo_handlers.cpp`). Captures decisions that aren't obvious from the code.

## Net-new, not a port — and a third-party message

The official plugin repo is a *mechanical port* of upstream PlotJuggler (see
`porting_guide.md`). This handler is **not** a port: upstream PlotJuggler has no
YOLO support. It is net-new, added on the same footing as the other new
canonical-object handlers that exist purely as groundwork for PJ4's object
pipeline (Marker→SceneEntities, OccupancyGrid, FrameTransforms), following the
documented "Adding a new schema" procedure in `README.md`.

It is also the first handler for a **third-party** message:
`yolo_msgs` comes from `github.com/mgonzs13/yolo_ros`, not from standard ROS.
Every other handler targets a standard `*_msgs` package. Whether this belongs in
the official parser long-term is an open question — kept here on a local branch.

## Canonical target: `kImageAnnotations` (2D overlay)

YOLO 2D detections live in **image-pixel coordinates**, so the natural canonical
type is `sdk::ImageAnnotations` (2D vector overlay), **not** `sdk::SceneEntities`
(which is 3D and would force a projection). The image base defines the canvas;
in PJ4's Scene2D the user stacks the annotations layer over the camera-image
layer — association is by layer stacking, not by matching `image_topic` (which we
fill informationally from the message `header.frame_id`, the only hint available).

Per detection:
| YOLO field | → annotation |
|---|---|
| `bbox` (center + size, px) | `PointsAnnotation` `kLineLoop` of 4 corners (`center ± size/2`) |
| `class_name` + `score` | `TextAnnotation` at the box top-left corner |
| `mask.data` (boundary points) | `PointsAnnotation` `kLineLoop` (the contour) |
| `keypoints` (2D) | one small filled `CircleAnnotation` per keypoint |

Color is cycled by `class_id` through a fixed high-contrast palette.

## Decode is positional CDR — every field consumed in full

Like the Marker handler, the decode reads the CDR wire positionally
(`deserializer_->deserialize(TYPE).convert<>()`, `deserializeUInt32`,
`deserializeString`). **Every field of every Detection is consumed even when not
rendered** (`theta`, the whole `bbox3d`, `mask` height/width, `keypoints3d`) — a
`Detection[]` is sequential, so skipping a field would desync the next element.
The field order is validated against the schema embedded in a real ROS 2 bag
(`yolo_msgs/Detection`: class_id, class_name, score, id, bbox, bbox3d, mask,
keypoints, keypoints3d — `Mask` = height, width, Point2D[] data).

## Stateless, like all object handlers

`parseObject` is a pure, lazy, idempotent decoder: each message → a self-contained
`ImageAnnotations` snapshot. No cross-message state (see `MARKER_NOTES.md` for the
full rationale on why statefulness is a consumer concern, not a parser concern).

## Object-only ingest

Registered with `object_type = kImageAnnotations` and **no `parse_scalars`** — YOLO
detections are overlays, not plottable columns. The PJ4 host must set
`kImageAnnotations → kPureLazy` (the eager-scalars default would abort the push on
an object-only handler).
