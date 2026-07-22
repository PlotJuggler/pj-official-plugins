#include "ros_manifest.hpp"
#include "ros_parser_dialog.hpp"
#include "ros_parser_internal.hpp"

// Newer SDKs let the exported symbol be named separately from the plugin type.
// Keep the alias fallback until that API is available in a released SDK.
#ifdef PJ_MESSAGE_PARSER_PLUGIN_NAMED
PJ_MESSAGE_PARSER_PLUGIN_NAMED(ros_parser_detail::RosParser, RosParserPlugin, kRosManifest)
#elif defined(PJ_STATIC_PLUGINS)
using RosParserPlugin = ros_parser_detail::RosParser;
PJ_MESSAGE_PARSER_PLUGIN(RosParserPlugin, kRosManifest)
#else
PJ_MESSAGE_PARSER_PLUGIN(ros_parser_detail::RosParser, kRosManifest)
#endif
PJ_DIALOG_PLUGIN(RosParserDialog, kRosManifest)
