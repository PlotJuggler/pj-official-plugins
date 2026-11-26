# parser_ros: yolo_msgs/DetectionArray -> ImageAnnotations

Design notes for the YOLO detection -> `PJ::sdk::ImageAnnotations` conversion
(`ros_yolo_handlers.cpp`).

`yolo_msgs` comes from `github.com/mgonzs13/yolo_ros`, not from standard ROS, and has no
PlotJuggler 3 equivalent. It is the only handler for a third-party message.

## Canonical target: `kImageAnnotations` (2D overlay)

YOLO 2D detections live in **image-pixel coordinates**, so the canonical type is
`sdk::ImageAnnotations` (2D vector overlay), not `sdk::SceneEntities` (3D, which would force a
projection). The image base defines the canvas; in Scene2D the user stacks the annotations layer over
the camera-image layer. Association is by layer stacking, not by matching `image_topic`, which is
filled informationally from the message `header.frame_id` (the only hint available).

Per detection:

| YOLO field | Annotation |
|---|---|
| `bbox` (center + size, px) | `PointsAnnotation` `kLineLoop` of 4 corners (`center ± size/2`) |
| `class_name` + `score` | `TextAnnotation` at the box top-left corner |
| `mask.data` (boundary points) | `PointsAnnotation` `kLineLoop` (the contour) |
| `keypoints` (2D) | one small filled `CircleAnnotation` per keypoint |

Color is cycled by `class_id` through a fixed high-contrast palette.

## Decode is positional CDR: every field is consumed in full

Like the Marker handler, the decode reads the CDR wire positionally. **Every field of every
Detection is consumed even when not rendered** (`theta`, the whole `bbox3d`, `mask` height/width,
`keypoints3d`): a `Detection[]` is sequential, so skipping a field would desync the next element.
The field order matches the schema embedded in a real ROS 2 bag (`yolo_msgs/Detection`: class_id,
class_name, score, id, bbox, bbox3d, mask, keypoints, keypoints3d; `Mask` = height, width,
Point2D[] data).

Array counts are capped and `reserve()` is clamped by the bytes left, so a corrupt count cannot
amplify an allocation. A corrupt-but-in-bounds count can still desync the decode without overrunning;
trailing bytes after the array are warned about once and the result is returned best-effort.

## Stateless

`parseObject` is a pure, lazy, idempotent decoder: each message gives a self-contained
`ImageAnnotations` snapshot, with no cross-message state (see `MARKER_NOTES.md` for why statefulness
is a consumer concern).

## Scalar route

The object is registered with a slim `parse_scalars` (`parseYoloScalars`: the header series plus
`num_detections`). An object-only handler would make the host's eager-scalar ingest abort the push on
any non-`kPureLazy` policy, silently dropping the overlay.
