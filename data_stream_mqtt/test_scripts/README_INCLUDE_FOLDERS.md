# Testing Protobuf Include Folders

This directory contains test cases for verifying the include folder functionality in PlotJuggler's Protobuf parser.

## Test Case: robot_pose.proto with imports

The `robot_pose.proto` file imports types from `common/types.proto`. This requires configuring include folders in PlotJuggler.

### File Structure

```
test_scripts/
├── robot_pose.proto          # Main proto - imports from common/
├── common/
│   └── types.proto           # Shared types (Vector3, Quaternion, Header)
├── mqtt_test_robot_pose.py   # Publisher script
└── robot_pose_pb2.py         # Generated Python module
```

### How to Test

1. **Start MQTT broker:**
   ```bash
   mosquitto -p 1883
   ```

2. **Run the publisher:**
   ```bash
   cd test_scripts
   python3 mqtt_test_robot_pose.py
   ```

3. **In PlotJuggler:**
   - Open Streaming → MQTT
   - Configure connection (localhost:1883)
   - Select **protobuf** encoding
   - Click **Load .proto file** and select `robot_pose.proto`
   - **IMPORTANT:** Click **Add folder...** in the "Include Folders" tab
   - Select the `test_scripts/` directory (parent of `common/`)
   - Select message type: `pj.test.RobotPose`
   - Optionally enable "use embedded timestamp"
   - Click OK and subscribe to topic `robot/pose`

4. **Expected Result:**
   - Without include folder: Compilation fails with "Import not found: common/types.proto"
   - With include folder: Compilation succeeds, all fields visible in the tree

### Fields in RobotPose

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

### Why This Test Matters

This test verifies that:
1. `setFolderPicker()` SDK API works correctly
2. `DiskSourceTree.MapPath()` resolves imports from include folders
3. Transitive imports are handled (if common/types.proto had its own imports)
4. The compiled `FileDescriptorSet` includes all dependent descriptors

### Related Code

- `protobuf_parser_dialog.hpp:262-265` — where include folders are added to source tree
- `sdk_file_folder_picker.md` — SDK contribution documentation
