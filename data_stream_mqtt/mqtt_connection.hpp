#pragma once

#include <mqtt/async_client.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <pj_streaming/endpoint.hpp>
#include <string>

namespace pj::mqtt_support {

/// Connection settings shared by dialog-time discovery and runtime streaming.
struct ConnectionSettings {
  std::string address = "localhost";
  int port = 1883;
  std::string username;
  std::string password;
  int protocol_version = 1;  // 0=3.1, 1=3.1.1, 2=5.0
  bool use_ssl = false;
  std::string ca_cert_path;
  std::string client_cert_path;
  std::string private_key_path;
};

[[nodiscard]] inline ConnectionSettings connectionSettingsFromJson(const nlohmann::json& cfg) {
  ConnectionSettings settings;
  if (!cfg.is_object()) {
    return settings;
  }
  settings.address = cfg.value("address", settings.address);
  settings.port = cfg.value("port", settings.port);
  settings.username = cfg.value("username", std::string{});
  settings.password = cfg.value("password", std::string{});
  settings.protocol_version = cfg.value("protocol_version", settings.protocol_version);
  settings.use_ssl = cfg.value("use_ssl", false);
  settings.ca_cert_path = cfg.value("ca_cert_path", std::string{});
  settings.client_cert_path = cfg.value("client_cert_path", std::string{});
  settings.private_key_path = cfg.value("private_key_path", std::string{});
  return settings;
}

[[nodiscard]] inline std::string brokerUri(const ConnectionSettings& settings) {
  return pj::streaming::composeEndpoint(
      settings.use_ssl ? "ssl" : "tcp", settings.address, std::to_string(settings.port));
}

[[nodiscard]] inline mqtt::connect_options makeConnectOptions(const ConnectionSettings& settings) {
  mqtt::connect_options options;
  options.set_clean_session(true);
  options.set_connect_timeout(std::chrono::seconds(5));
  if (settings.protocol_version == 0) {
    options.set_mqtt_version(MQTTVERSION_3_1);
  } else if (settings.protocol_version == 2) {
    options.set_mqtt_version(MQTTVERSION_5);
  } else {
    options.set_mqtt_version(MQTTVERSION_3_1_1);
  }
  if (!settings.username.empty()) {
    options.set_user_name(settings.username);
    options.set_password(settings.password);
  }
  if (settings.use_ssl) {
    mqtt::ssl_options ssl_options;
    if (!settings.ca_cert_path.empty()) {
      ssl_options.set_trust_store(settings.ca_cert_path);
    }
    if (!settings.client_cert_path.empty()) {
      ssl_options.set_key_store(settings.client_cert_path);
    }
    if (!settings.private_key_path.empty()) {
      ssl_options.set_private_key(settings.private_key_path);
    }
    options.set_ssl(ssl_options);
  }
  return options;
}

}  // namespace pj::mqtt_support
