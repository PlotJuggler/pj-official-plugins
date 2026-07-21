#include "ros_manifest.hpp"
#include "ros_parser_dialog.hpp"
#include "ros_parser_internal.hpp"

// Static plugin exports token-paste the class name into the getter symbol, so
// a qualified type is not a valid macro argument. Keep the dynamic export
// unchanged and give static consumers an unqualified local alias.
#ifdef PJ_STATIC_PLUGINS
using RosParserPlugin = ros_parser_detail::RosParser;
PJ_MESSAGE_PARSER_PLUGIN(RosParserPlugin, kRosManifest)
#else
PJ_MESSAGE_PARSER_PLUGIN(ros_parser_detail::RosParser, kRosManifest)
#endif
PJ_DIALOG_PLUGIN(RosParserDialog, kRosManifest)
