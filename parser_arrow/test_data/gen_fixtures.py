"""Generate deterministic Arrow IPC stream fixtures for parser_arrow tests.

Run from the repository root with PyArrow 24:

    python3 -m pip install pyarrow  # any interpreter with pyarrow >= 15 (view types)
    python3 parser_arrow/test_data/gen_fixtures.py

Every output uses the Arrow IPC stream format (``pa.ipc.new_stream``), not the
Arrow IPC file format. Generated files are intentionally checked into the
repository so normal C++ test runs do not require Python or PyArrow.
"""

import datetime
import decimal
from pathlib import Path

import pyarrow as pa


OUTPUT_DIR = Path(__file__).resolve().parent


def write_stream(
    filename: str,
    batches: list[pa.RecordBatch],
    options: pa.ipc.IpcWriteOptions | None = None,
) -> Path:
    """Write record batches as one Arrow IPC stream and return its path."""
    output_path = OUTPUT_DIR / filename
    with output_path.open("wb") as output_file:
        with pa.ipc.new_stream(output_file, batches[0].schema, options=options) as writer:
            for batch in batches:
                writer.write_batch(batch)
    return output_path


def make_flat_batch() -> pa.RecordBatch:
    """Build the shared four-column flat fixture batch."""
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000], type=pa.int64()),
            pa.array([1.5, 2.5, 3.5], type=pa.float64()),
            pa.array([10, 20, 30], type=pa.int32()),
            pa.array(["x", "y", "z"], type=pa.string()),
        ],
        names=["timestamp_ns", "a", "b", "name"],
    )


def make_nested_batch() -> pa.RecordBatch:
    """Build nested pose data with values documented for flattening tests."""
    position_type = pa.struct(
        [
            pa.field("x", pa.float64()),
            pa.field("y", pa.float64()),
            pa.field("z", pa.float64()),
        ]
    )
    orientation_type = pa.struct(
        [
            pa.field("x", pa.float64()),
            pa.field("y", pa.float64()),
            pa.field("z", pa.float64()),
            pa.field("w", pa.float64()),
        ]
    )
    pose_type = pa.struct(
        [
            pa.field("position", position_type),
            pa.field("orientation", orientation_type),
        ]
    )

    # Row 0: position=(1, 2, 3), orientation=(0.1, 0.2, 0.3, 0.4), speed=5.5.
    # Row 1: position=(4, 5, 6), orientation=(0.5, 0.6, 0.7, 0.8), speed=6.5.
    # Row 2: null pose, speed=7.5. This exercises parent-null propagation.
    poses = pa.array(
        [
            {
                "position": {"x": 1.0, "y": 2.0, "z": 3.0},
                "orientation": {"x": 0.1, "y": 0.2, "z": 0.3, "w": 0.4},
            },
            {
                "position": {"x": 4.0, "y": 5.0, "z": 6.0},
                "orientation": {"x": 0.5, "y": 0.6, "z": 0.7, "w": 0.8},
            },
            None,
        ],
        type=pose_type,
    )
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000], type=pa.int64()),
            poses,
            pa.array([5.5, 6.5, 7.5], type=pa.float64()),
        ],
        names=["timestamp_ns", "pose", "speed"],
    )


def make_views_batch() -> pa.RecordBatch:
    """Build string_view and binary_view data with inline and external values."""
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000], type=pa.int64()),
            pa.array(
                ["short", "this string is longer than twelve bytes", "end"],
                type=pa.string_view(),
            ),
            pa.array(
                [b"\x01\x02", b"0123456789abcdef", b"\xff"],
                type=pa.binary_view(),
            ),
            pa.array([1.25, 2.25, 3.25], type=pa.float64()),
        ],
        names=["timestamp_ns", "label", "blob", "value"],
    )


def make_views_storage_batch() -> pa.RecordBatch:
    """Build canonical storage for the C-Data view normalization test."""
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000, 4000], type=pa.int64()),
            pa.array(
                ["short", "this string is longer than twelve bytes", None, "end"],
                type=pa.string(),
            ),
            pa.array(
                [b"\x01\x02", b"0123456789abcdef", None, b"\xff"],
                type=pa.binary(),
            ),
            pa.array([1.25, 2.25, 3.25, 4.25], type=pa.float64()),
        ],
        names=["timestamp_ns", "label", "blob", "value"],
    )


def make_no_timestamp_batch() -> pa.RecordBatch:
    """Build a flat batch with no timestamp candidate."""
    return pa.record_batch(
        [
            pa.array([1.5, 2.5, 3.5], type=pa.float64()),
            pa.array([10, 20, 30], type=pa.int32()),
        ],
        names=["a", "b"],
    )


def make_timestamp_typed_batch() -> pa.RecordBatch:
    """Build a typed timestamp column alongside an int64 name-heuristic decoy."""
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000], type=pa.timestamp("ns")),
            pa.array([9000, 8000, 7000], type=pa.int64()),
            pa.array([1.0, 2.0, 3.0], type=pa.float64()),
        ],
        names=["stamp", "time", "v"],
    )


def make_timestamp_casts_batch() -> pa.RecordBatch:
    """Build millisecond axis and microsecond data timestamps for cast tests."""
    return pa.record_batch(
        [
            pa.array([1, 2, 3], type=pa.timestamp("ms")),
            pa.array([4, None, 6], type=pa.timestamp("us")),
            pa.array([1.0, 2.0, 3.0], type=pa.float64()),
        ],
        names=["stamp", "recording_timestamp_ns", "value"],
    )


def make_large_types_batch() -> pa.RecordBatch:
    """Build large string and binary columns for canonical-width normalization."""
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000], type=pa.int64()),
            pa.array(["short", "this is a large string value", None], type=pa.large_string()),
            pa.array([b"a", b"large binary value", None], type=pa.large_binary()),
            pa.array([1.0, 2.0, 3.0], type=pa.float64()),
        ],
        names=["timestamp_ns", "label", "blob", "value"],
    )


def make_axis_batch(axis_type: pa.DataType, values: list[object]) -> pa.RecordBatch:
    """Build one named time axis and one host-ingestible data column."""
    return pa.record_batch(
        [pa.array(values, type=axis_type), pa.array([1.0, 2.0, 3.0], type=pa.float64())],
        names=["time", "value"],
    )


def make_nested_dropped_scalars_batch() -> pa.RecordBatch:
    """Build nullable nested date/decimal leaves that the host will skip."""
    metadata_type = pa.struct(
        [
            pa.field("date", pa.date32()),
            pa.field("amount", pa.decimal128(10, 2)),
        ]
    )
    metadata = pa.array(
        [
            {"date": datetime.date(2020, 1, 2), "amount": decimal.Decimal("12.34")},
            None,
            {"date": datetime.date(2020, 1, 4), "amount": decimal.Decimal("-5.67")},
        ],
        type=metadata_type,
    )
    return pa.record_batch(
        [
            pa.array([1000, 2000, 3000], type=pa.int64()),
            metadata,
            pa.array([1.0, 2.0, 3.0], type=pa.float64()),
        ],
        names=["timestamp_ns", "metadata", "value"],
    )


def main() -> None:
    """Generate every checked-in test fixture and print its byte size."""
    flat_batch = make_flat_batch()
    generated_paths = [
        write_stream("flat.arrows", [flat_batch]),
        write_stream(
            "flat_two_batches.arrows",
            [flat_batch.slice(0, 2), flat_batch.slice(2, 1)],
        ),
        write_stream("nested.arrows", [make_nested_batch()]),
        write_stream("views.arrows", [make_views_batch()]),
        write_stream("views_storage.arrows", [make_views_storage_batch()]),
        write_stream(
            "flat_zstd.arrows",
            [flat_batch],
            pa.ipc.IpcWriteOptions(compression="zstd"),
        ),
        write_stream(
            "flat_lz4.arrows",
            [flat_batch],
            pa.ipc.IpcWriteOptions(compression="lz4"),
        ),
        write_stream("timestamp_typed.arrows", [make_timestamp_typed_batch()]),
        write_stream("timestamp_casts.arrows", [make_timestamp_casts_batch()]),
        write_stream("large_types.arrows", [make_large_types_batch()]),
        write_stream("axis_int32.arrows", [make_axis_batch(pa.int32(), [1, 2, 3])]),
        write_stream("axis_uint32.arrows", [make_axis_batch(pa.uint32(), [1, 2, 3])]),
        write_stream("axis_uint64_overflow.arrows", [make_axis_batch(pa.uint64(), [1, 2**63, 3])]),
        write_stream("axis_float.arrows", [make_axis_batch(pa.float32(), [1.5, 2.25, -0.5])]),
        write_stream("axis_double.arrows", [make_axis_batch(pa.float64(), [1.5, 2.25, -0.5])]),
        write_stream("axis_fractional_ns.arrows", [make_axis_batch(pa.float64(), [1.6e-9, -1.6e-9, 2.4e-9])]),
        write_stream("axis_double_nonfinite.arrows", [make_axis_batch(pa.float64(), [1.0, float("nan"), 3.0])]),
        write_stream("axis_null.arrows", [make_axis_batch(pa.int64(), [1, None, 3])]),
        write_stream(
            "timestamp_seconds_overflow.arrows",
            [make_axis_batch(pa.timestamp("s"), [1, 2**63 - 1, 3])],
        ),
        write_stream("nested_dropped_scalars.arrows", [make_nested_dropped_scalars_batch()]),
        write_stream("no_timestamp.arrows", [make_no_timestamp_batch()]),
        write_stream(
            "no_timestamp_two_batches.arrows",
            [make_no_timestamp_batch().slice(0, 2), make_no_timestamp_batch().slice(2, 1)],
        ),
    ]

    for output_path in generated_paths:
        print(f"{output_path.name}: {output_path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
