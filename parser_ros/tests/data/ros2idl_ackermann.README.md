# ROS2 IDL regression fixture

`ros2idl_ackermann.idl` is the complete schema and `ros2idl_ackermann.cdr` is
the first 20-byte message from the recording attached to
[PlotJuggler issue #851](https://github.com/PlotJuggler/PlotJuggler/issues/851):
`rosbag2_idl_example.mcap` from the reporter's ZIP attachment.

The schema name is `autoware_auto_control_msgs/msg/AckermannLateralCommand`,
encoding `ros2idl`. The payload has stamp seconds/nanoseconds both zero,
`steering_tire_angle = 0.5`, and `steering_tire_rotation_rate = -0.5`.
