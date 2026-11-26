# Testing Protobuf Include Folders

`robot_pose.proto` imports `common/types.proto`, so loading it in the Protobuf parser needs
`test_scripts/` added as an include folder.

```
test_scripts/
├── robot_pose.proto          # imports common/types.proto
├── common/types.proto        # Vector3, Quaternion, Header
└── mqtt_test_robot_pose.py   # publisher; compiles the protos with protoc on first run
```

## How to test

1. Start a broker: `mosquitto -p 1883`
2. Run the publisher: `cd test_scripts && python3 mqtt_test_robot_pose.py`
3. In PlotJuggler:
   - Open Streaming → MQTT and connect to `localhost:1883`
   - Select **protobuf** encoding and **Load .proto file** `robot_pose.proto`
   - In the "Include Folders" tab, **Add folder...** and select `test_scripts/` (the parent of `common/`)
   - Select message type `pj.test.RobotPose`; optionally enable "use embedded timestamp"
   - Click OK and subscribe to topic `robot/pose`

Without the include folder, compilation fails with "Import not found: common/types.proto". With it, all
fields appear in the tree.

## Fields in RobotPose

| Field | Type | Description |
|-------|------|-------------|
| `header.timestamp` | double | Embedded timestamp (seconds) |
| `header.frame_id` | string | Coordinate frame |
| `header.seq` | uint32 | Sequence number |
| `position.x/y/z` | double | Circular motion |
| `orientation.w/x/y/z` | double | Quaternion |
| `linear_velocity.x/y/z` | double | Velocity vector |
| `angular_velocity.x/y/z` | double | Angular velocity |
| `robot_name` | string | Always "test_robot" |
| `is_moving` | bool | Always true |
| `battery_level` | double | Decreasing 100→0 |
