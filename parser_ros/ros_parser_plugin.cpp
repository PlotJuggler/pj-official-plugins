#include "ros_manifest.hpp"
#include "ros_parser_dialog.hpp"
#include "ros_parser_internal.hpp"

PJ_MESSAGE_PARSER_PLUGIN(ros_parser_detail::RosParser, kRosManifest)
PJ_DIALOG_PLUGIN(RosParserDialog)
