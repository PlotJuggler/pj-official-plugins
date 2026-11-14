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

# Fingerprint of the geometry (NavMap::geometry_id(); 0 = unspecified), as in NavMapLayer
uint64 geometry_id

# Links between NavCels that are not adjacent (file format version 2)
navmap_ros_interfaces/NavMapLink[] links

================================================================================
MSG: navmap_ros_interfaces/NavMapLayer
# Per-NavCel layer (one scalar per triangle)
std_msgs/Header header

uint8 U8 = 0
uint8 F32 = 1
uint8 F64 = 2
uint8 I16 = 3

string name
uint8 type         # 0=U8, 1=F32, 2=F64, 3=I16

# Metadata
string description
string unit
bool has_unknown        # true: unknown_value means "not observed" (NaN allowed)
float64 unknown_value

# Geometry this layer belongs to (NavMap::geometry_id(); 0 = unspecified)
uint64 geometry_id

# Sparse update: if non-empty, the data_* array holds one value per listed NavCel and the
# rest of the layer is unchanged. If empty, the data_* array covers every NavCel.
uint32[] indices

# Exactly one of the data_* arrays must be non-empty, and its length must be
# equal to the number of triangles (NavCels) in the NavMap message (or to indices).
uint8[]   data_u8
float32[] data_f32
float64[] data_f64
int16[]   data_i16

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
MSG: navmap_ros_interfaces/NavMapLink
# Link between two NavCels that do not share an edge (stairs, elevator, door...).
# The map only records it: whether a robot can take it is up to the planner.

uint8 GENERIC = 0
uint8 RAMP = 1
uint8 STAIRS = 2
uint8 ELEVATOR = 3
uint8 DOOR = 4

uint32 from_navcel
uint32 to_navcel
uint8 type
float32 length        # travel length (m)
bool bidirectional    # also from to_navcel to from_navcel

================================================================================
MSG: navmap_ros_interfaces/NavMapSurface
# A surface groups a subset of triangles in a frame.

string frame_id
uint32[] navcels
