#include "ros_manifest.hpp"
#include "ros_parser_dialog.hpp"
#include "ros_parser_internal.hpp"

// PJ_MESSAGE_PARSER_PLUGIN_NAMED (newer SDKs) names the exported symbol separately from
// the plugin type; the other branches are the fallback for SDKs without it.
#ifdef PJ_MESSAGE_PARSER_PLUGIN_NAMED
PJ_MESSAGE_PARSER_PLUGIN_NAMED(ros_parser_detail::RosParser, RosParserPlugin, kRosManifest)
#elif defined(PJ_STATIC_PLUGINS)
using RosParserPlugin = ros_parser_detail::RosParser;
PJ_MESSAGE_PARSER_PLUGIN(RosParserPlugin, kRosManifest)
#else
PJ_MESSAGE_PARSER_PLUGIN(ros_parser_detail::RosParser, kRosManifest)
#endif
PJ_DIALOG_PLUGIN(RosParserDialog, kRosManifest)
