#include "ros2_trace_model/fs_trace_source.hpp"

#include <babeltrace2/babeltrace.h>

#include <filesystem>
#include <utility>

namespace ros2_trace_model {

namespace {

// Convert one scalar babeltrace2 field to a FieldValue. Non-scalar field
// classes (arrays such as the DDS gid, nested structs) are not used by any
// deriver, so they map to monostate and are skipped by the caller.
FieldValue toFieldValue(const bt_field* field) {
  const bt_field_class_type type = bt_field_get_class_type(field);
  if (bt_field_class_type_is(type, BT_FIELD_CLASS_TYPE_BOOL)) {
    return static_cast<bool>(bt_field_bool_get_value(field));
  }
  if (bt_field_class_type_is(type, BT_FIELD_CLASS_TYPE_UNSIGNED_INTEGER)) {
    return static_cast<std::uint64_t>(bt_field_integer_unsigned_get_value(field));
  }
  if (bt_field_class_type_is(type, BT_FIELD_CLASS_TYPE_SIGNED_INTEGER)) {
    return static_cast<std::int64_t>(bt_field_integer_signed_get_value(field));
  }
  if (bt_field_class_type_is(type, BT_FIELD_CLASS_TYPE_STRING)) {
    const char* str = bt_field_string_get_value(field);
    return std::string(str != nullptr ? str : "");
  }
  return std::monostate{};
}

// Read cpu_id out of the packet context, if the stream has packets and a
// cpu_id member (LTTng per-CPU sub-buffers expose it there).
std::optional<std::uint32_t> readCpu(const bt_event* event) {
  const bt_stream* stream = bt_event_borrow_stream_const(event);
  const bt_stream_class* stream_class = bt_stream_borrow_class_const(stream);
  if (!bt_stream_class_supports_packets(stream_class)) {
    return std::nullopt;
  }
  const bt_packet* packet = bt_event_borrow_packet_const(event);
  if (packet == nullptr) {
    return std::nullopt;
  }
  const bt_field* context = bt_packet_borrow_context_field_const(packet);
  if (context == nullptr) {
    return std::nullopt;
  }
  const bt_field* cpu = bt_field_structure_borrow_member_field_by_name_const(context, "cpu_id");
  if (cpu == nullptr) {
    return std::nullopt;
  }
  if (!bt_field_class_type_is(bt_field_get_class_type(cpu), BT_FIELD_CLASS_TYPE_UNSIGNED_INTEGER)) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(bt_field_integer_unsigned_get_value(cpu));
}

RawEvent decodeEvent(const bt_message* message) {
  const bt_event* event = bt_message_event_borrow_event_const(message);
  const char* name = bt_event_class_get_name(bt_event_borrow_class_const(event));
  const Tp tp = name != nullptr ? classifyTracepoint(name) : Tp::Other;

  std::int64_t ts_ns = 0;
  const bt_clock_snapshot* clock = bt_message_event_borrow_default_clock_snapshot_const(message);
  if (clock != nullptr) {
    bt_clock_snapshot_get_ns_from_origin(clock, &ts_ns);
  }

  std::vector<NamedField> fields;
  const bt_field* payload = bt_event_borrow_payload_field_const(event);
  if (payload != nullptr) {
    const bt_field_class* payload_class = bt_field_borrow_class_const(payload);
    const std::uint64_t count = bt_field_class_structure_get_member_count(payload_class);
    fields.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
      const bt_field_class_structure_member* member =
          bt_field_class_structure_borrow_member_by_index_const(payload_class, i);
      const char* member_name = bt_field_class_structure_member_get_name(member);
      const bt_field* member_field = bt_field_structure_borrow_member_field_by_index_const(payload, i);
      FieldValue value = toFieldValue(member_field);
      if (!std::holds_alternative<std::monostate>(value) && member_name != nullptr) {
        fields.push_back(NamedField{member_name, std::move(value)});
      }
    }
  }

  return RawEvent(tp, ts_ns, std::move(fields), readCpu(event));
}

// Simple-sink consume callback: drain the muxed message iterator, decoding
// every EVENT message into the output vector.
bt_graph_simple_sink_component_consume_func_status consume(bt_message_iterator* iterator, void* user_data) {
  auto* out = static_cast<std::vector<RawEvent>*>(user_data);

  bt_message_array_const messages = nullptr;
  uint64_t count = 0;
  const bt_message_iterator_next_status status = bt_message_iterator_next(iterator, &messages, &count);

  switch (status) {
    case BT_MESSAGE_ITERATOR_NEXT_STATUS_OK:
      for (uint64_t i = 0; i < count; ++i) {
        if (bt_message_get_type(messages[i]) == BT_MESSAGE_TYPE_EVENT) {
          out->push_back(decodeEvent(messages[i]));
        }
        bt_message_put_ref(messages[i]);
      }
      return BT_GRAPH_SIMPLE_SINK_COMPONENT_CONSUME_FUNC_STATUS_OK;
    case BT_MESSAGE_ITERATOR_NEXT_STATUS_END:
      return BT_GRAPH_SIMPLE_SINK_COMPONENT_CONSUME_FUNC_STATUS_END;
    default:
      return BT_GRAPH_SIMPLE_SINK_COMPONENT_CONSUME_FUNC_STATUS_ERROR;
  }
}

}  // namespace

FsTraceSource::FsTraceSource(std::string trace_dir) {
  load(trace_dir);
}

void FsTraceSource::load(const std::string& trace_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(fs::path(trace_dir) / "metadata", ec)) {
    error_ = "no CTF metadata found under: " + trace_dir;
    return;
  }

  const bt_plugin* ctf_plugin = nullptr;
  const bt_plugin* utils_plugin = nullptr;
  if (bt_plugin_find("ctf", BT_TRUE, BT_FALSE, BT_TRUE, BT_TRUE, BT_TRUE, &ctf_plugin) != BT_PLUGIN_FIND_STATUS_OK ||
      bt_plugin_find("utils", BT_TRUE, BT_FALSE, BT_TRUE, BT_TRUE, BT_TRUE, &utils_plugin) !=
          BT_PLUGIN_FIND_STATUS_OK) {
    error_ = "babeltrace2 ctf/utils plugins not found";
    if (ctf_plugin != nullptr) {
      bt_plugin_put_ref(ctf_plugin);
    }
    return;
  }

  const bt_component_class_source* source_class =
      bt_plugin_borrow_source_component_class_by_name_const(ctf_plugin, "fs");
  const bt_component_class_filter* muxer_class =
      bt_plugin_borrow_filter_component_class_by_name_const(utils_plugin, "muxer");

  bt_value* params = bt_value_map_create();
  bt_value* inputs = nullptr;
  bt_value_map_insert_empty_array_entry(params, "inputs", &inputs);
  bt_value_array_append_string_element(inputs, trace_dir.c_str());

  bt_graph* graph = bt_graph_create(0);
  const bt_component_source* source = nullptr;
  const bt_component_filter* muxer = nullptr;
  const bt_component_sink* sink = nullptr;

  bool failed = false;
  if (bt_graph_add_source_component(graph, source_class, "source", params, BT_LOGGING_LEVEL_WARNING, &source) !=
          BT_GRAPH_ADD_COMPONENT_STATUS_OK ||
      bt_graph_add_filter_component(graph, muxer_class, "muxer", nullptr, BT_LOGGING_LEVEL_WARNING, &muxer) !=
          BT_GRAPH_ADD_COMPONENT_STATUS_OK ||
      bt_graph_add_simple_sink_component(graph, "sink", nullptr, consume, nullptr, &events_, &sink) !=
          BT_GRAPH_ADD_COMPONENT_STATUS_OK) {
    error_ = "failed to add babeltrace2 components";
    failed = true;
  }

  // source.ctf.fs exposes one output port per stream; connect each to a fresh
  // muxer input port (the muxer grows a new input port as each is connected).
  if (!failed) {
    const std::uint64_t port_count = bt_component_source_get_output_port_count(source);
    for (std::uint64_t i = 0; i < port_count && !failed; ++i) {
      const bt_port_output* out_port = bt_component_source_borrow_output_port_by_index_const(source, i);
      const bt_port_input* in_port = bt_component_filter_borrow_input_port_by_index_const(muxer, i);
      if (bt_graph_connect_ports(graph, out_port, in_port, nullptr) != BT_GRAPH_CONNECT_PORTS_STATUS_OK) {
        error_ = "failed to connect source to muxer";
        failed = true;
      }
    }
  }

  if (!failed) {
    const bt_port_output* muxer_out = bt_component_filter_borrow_output_port_by_index_const(muxer, 0);
    const bt_port_input* sink_in = bt_component_sink_borrow_input_port_by_index_const(sink, 0);
    if (bt_graph_connect_ports(graph, muxer_out, sink_in, nullptr) != BT_GRAPH_CONNECT_PORTS_STATUS_OK) {
      error_ = "failed to connect muxer to sink";
      failed = true;
    }
  }

  if (!failed && bt_graph_run(graph) != BT_GRAPH_RUN_STATUS_OK) {
    error_ = "babeltrace2 graph run failed";
    events_.clear();
  }

  bt_graph_put_ref(graph);
  bt_value_put_ref(params);
  bt_plugin_put_ref(ctf_plugin);
  bt_plugin_put_ref(utils_plugin);
}

std::optional<RawEvent> FsTraceSource::next() {
  if (index_ >= events_.size()) {
    return std::nullopt;
  }
  return std::move(events_[index_++]);
}

}  // namespace ros2_trace_model
