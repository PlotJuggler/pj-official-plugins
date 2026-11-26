#pragma once

/// @file ros2_schema_builder.hpp
/// Builds the `.msg` schema `parser_ros` consumes (root type plus every nested type, `===` separators, the shape
/// rosbag2 stores) from the installed ROS's C++ introspection typesupport, so no .msg files are needed on disk.

#include <rclcpp/typesupport_helpers.hpp>
#include <rosidl_typesupport_introspection_cpp/field_types.hpp>
#include <rosidl_typesupport_introspection_cpp/message_introspection.hpp>
#include <set>
#include <string>

#include "../../ros2_type_name.hpp"

namespace ros2_streamer {

namespace detail {

// humble only has `get_typesupport_handle`; jazzy onward deprecates it in favor of `get_message_typesupport_handle`,
// which breaks the build under -Werror=deprecated-declarations. CMake passes -DROS_DISTRO_<UPPER>; an unknown future
// distro is assumed to follow the newer naming.
inline const rosidl_message_type_support_t* getMessageTypesupportHandle(
    const std::string& type_name, const std::string& typesupport_identifier, rcpputils::SharedLibrary& library) {
#if defined(ROS_DISTRO_HUMBLE)
  return rclcpp::get_typesupport_handle(type_name, typesupport_identifier, library);
#else
  return rclcpp::get_message_typesupport_handle(type_name, typesupport_identifier, library);
#endif
}

}  // namespace detail

inline std::string buildRos2Schema(const std::string& base_type) {
  using namespace rosidl_typesupport_introspection_cpp;

  std::string schema;
  std::set<std::string> pending;
  std::set<std::string> done;

  auto append_type = [&](const std::string& type_name, bool with_separator) {
    auto lib = rclcpp::get_typesupport_library(type_name, "rosidl_typesupport_introspection_cpp");
    auto support = detail::getMessageTypesupportHandle(type_name, "rosidl_typesupport_introspection_cpp", *lib);

    if (with_separator) {
      schema += "=====================================\nMSG: ";
      schema += schemaTypeName(type_name);
      schema += '\n';
    }

    const auto* members = static_cast<const MessageMembers*>(support->data);
    for (size_t i = 0; i < members->member_count_; ++i) {
      const MessageMember& member = members->members_[i];

      switch (member.type_id_) {
        case ROS_TYPE_FLOAT32:
          schema += "float32";
          break;
        case ROS_TYPE_FLOAT64:
          schema += "float64";
          break;
        case ROS_TYPE_UINT8:
        case ROS_TYPE_BYTE:
        case ROS_TYPE_CHAR:
          schema += "uint8";
          break;
        case ROS_TYPE_BOOLEAN:
          schema += "bool";
          break;
        case ROS_TYPE_INT8:
          schema += "int8";
          break;
        case ROS_TYPE_UINT16:
          schema += "uint16";
          break;
        case ROS_TYPE_INT16:
          schema += "int16";
          break;
        case ROS_TYPE_UINT32:
          schema += "uint32";
          break;
        case ROS_TYPE_INT32:
          schema += "int32";
          break;
        case ROS_TYPE_UINT64:
          schema += "uint64";
          break;
        case ROS_TYPE_INT64:
          schema += "int64";
          break;
        case ROS_TYPE_STRING:
        case ROS_TYPE_WSTRING:
          schema += "string";
          break;
        case ROS_TYPE_MESSAGE: {
          const auto* nested = reinterpret_cast<const MessageMembers*>(member.members_->data);
          const std::string nested_type = interfaceTypeName(nested->message_namespace_, nested->message_name_);
          schema += schemaTypeName(nested_type);
          if (done.count(nested_type) == 0) {
            pending.insert(nested_type);
          }
        } break;
      }

      if (member.is_array_) {
        if (member.array_size_ > 0) {
          schema += "[";
          schema += std::to_string(member.array_size_);
          schema += "]";
        } else {
          schema += "[]";
        }
      }
      schema += ' ';
      schema += member.name_;
      schema += '\n';
    }
  };

  append_type(base_type, false);

  while (!pending.empty()) {
    auto it = pending.begin();
    const std::string other = *it;
    pending.erase(it);
    done.insert(other);
    append_type(other, true);
  }
  return schema;
}

}  // namespace ros2_streamer
