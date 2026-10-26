// Command channels: serial (UART0) and WebSocket clients feed lines into one executor task
// that runs them through the shared Parser and routes the reply back to the sender.
#pragma once

#include <cstdint>
#include <string>

#include "core/Parser.h"

namespace ss::app {

// 0 = serial. A WebSocket channel carries its socket and a connection generation, so a
// late reply for a closed connection never reaches a new client that reuses the socket.
using ChannelId = uint32_t;
constexpr ChannelId kUart = 0;
constexpr ChannelId wsChannel(int fd, uint32_t generation) { return (generation << 8) | (fd & 0xFF); }
constexpr int wsFd(ChannelId ch) { return static_cast<int>(ch & 0xFF); }
constexpr uint32_t wsGeneration(ChannelId ch) { return ch >> 8; }

Parser& parser();

// Starts the executor and the UART reader, and routes broadcasts to all channels.
void startCommands();

// Queues a command line. Returns false if the executor stays busy; the caller must then
// resync the client (a reply out of order would be paired with the wrong request).
bool submitLine(ChannelId ch, const std::string& line);
void submitRealtime(ChannelId ch, char c);  // handled immediately, not queued

// WebSocket transport, provided by the web server.
using WsSend = void (*)(ChannelId ch, const std::string& text);
using WsBroadcast = void (*)(const std::string& text);
void setWsTransport(WsSend send, WsBroadcast broadcast);

// Number of log lines dropped because the serial output buffer was full.
uint32_t droppedLogLines();

}  // namespace ss::app
