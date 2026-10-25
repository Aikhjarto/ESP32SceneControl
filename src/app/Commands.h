// Command channels: serial (UART0) and WebSocket clients feed lines into one executor task
// that runs them through the shared Parser and routes the reply back to the sender.
#pragma once

#include <string>

#include "core/Parser.h"

namespace ss::app {

using ChannelId = int;
constexpr ChannelId kUart = 0;
constexpr ChannelId wsChannel(int fd) { return fd + 1; }

Parser& parser();

// Starts the executor and the UART reader, and routes broadcasts to all channels.
void startCommands();

void submitLine(ChannelId ch, const std::string& line);
void submitRealtime(ChannelId ch, char c);  // handled immediately, not queued

// WebSocket transport, provided by the web server.
using WsSend = void (*)(int fd, const std::string& text);
using WsBroadcast = void (*)(const std::string& text);
void setWsTransport(WsSend send, WsBroadcast broadcast);

}  // namespace ss::app
