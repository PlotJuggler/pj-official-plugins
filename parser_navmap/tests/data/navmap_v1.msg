# Compact NavMap transport.
std_msgs/Header header

# Vertex positions (SoA)
float32[] positions_x
float32[] positions_y
float32[] positions_z

# Optional per-vertex colors (RGBA). Fill only if has_vertex_rgba=true.
bool has_vertex_rgba
uint8[] colors_r
uint8[] colors_g
uint8[] colors_b
uint8[] colors_a

# Global triangle index buffers
uint32[] navcels_v0
uint32[] navcels_v1
uint32[] navcels_v2

# Surfaces (each lists indices into the global triangle arrays)
navmap_ros_interfaces/NavMapSurface[] surfaces

# Per-NavCel layers
navmap_ros_interfaces/NavMapLayer[] layers

================================================================================
MSG: navmap_ros_interfaces/NavMapLayer
# Per-NavCel layer (one scalar per triangle)
std_msgs/Header header

uint8 U8 = 0
uint8 F32 = 1
uint8 F64 = 2

string name
uint8 type         # 0=U8, 1=F32, 2=F64

# Exactly one of the data_* arrays must be non-empty, and its length must be
# equal to the number of triangles (NavCels) in the NavMap message.
uint8[]   data_u8
float32[] data_f32
float64[] data_f64

================================================================================
MSG: std_msgs/Header
# Standard metadata for higher-level stamped data types.
# This is generally used to communicate timestamped data
# in a particular coordinate frame.

# Two-integer timestamp that is expressed as seconds and nanoseconds.
builtin_interfaces/Time stamp

# Transform frame with which this data is associated.
string frame_id

================================================================================
MSG: builtin_interfaces/Time
# This message communicates ROS Time defined here:
# https://design.ros2.org/articles/clock_and_time.html

# The seconds component, valid over all int32 values.
int32 sec

# The nanoseconds component, valid in the range [0, 1e9), to be added to the seconds component.
# e.g.
# The time -1.7 seconds is represented as {sec: -2, nanosec: 3e8}
# The time 1.7 seconds is represented as {sec: 1, nanosec: 7e8}
uint32 nanosec

================================================================================
MSG: navmap_ros_interfaces/NavMapSurface
# A surface groups a subset of triangles in a frame.

string frame_id
uint32[] navcels
