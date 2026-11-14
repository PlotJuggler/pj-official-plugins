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
