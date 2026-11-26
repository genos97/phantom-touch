#include "bridge/ConnectionState.hpp"

int ConnectionStateMachine::backoffMs(int attempt) {
    if (attempt <= 0) return 1000;
    if (attempt == 1) return 2000;
    if (attempt == 2) return 4000;
    if (attempt == 3) return 8000;
    return 10000;
}

ConnState ConnectionStateMachine::evaluate(const ConnStateInput& in) {
    if (!in.toolsReady) return CONN_NO_TOOLS;
    if (in.streaming) return CONN_STREAMING;
    if (in.connecting) return CONN_CONNECTING;
    if (in.reconnecting && !in.reconnectCancelled) {
        if (in.reconnectAttempt > in.maxReconnectAttempts) return CONN_LOST;
        return CONN_RECONNECTING;
    }
    if (in.hasError) return CONN_ERROR;
    if (in.readyDeviceCount > 0) return CONN_READY;
    if (in.unauthorizedDeviceCount > 0) return CONN_UNAUTHORIZED;
    return CONN_NO_DEVICE;
}

const char* ConnectionStateMachine::stateTitle(ConnState st) {
    switch (st) {
        case CONN_NO_TOOLS:     return "Tools Missing";
        case CONN_NO_DEVICE:    return "No Device";
        case CONN_UNAUTHORIZED: return "Authorize Phone";
        case CONN_READY:        return "Device Ready";
        case CONN_CONNECTING:   return "Connecting";
        case CONN_STREAMING:    return "Streaming";
        case CONN_RECONNECTING: return "Reconnecting";
        case CONN_LOST:         return "Connection Lost";
        case CONN_ERROR:        return "Connection Error";
        default:                return "Disconnected";
    }
}

const char* ConnectionStateMachine::stepLabel(ConnectProgressStep step) {
    switch (step) {
        case CONN_STEP_PUSH_SERVER:  return "Pushing scrcpy server...";
        case CONN_STEP_START_SERVER: return "Starting video server...";
        case CONN_STEP_WAIT_VIDEO:   return "Opening H.264 stream...";
        default:                     return "Connecting...";
    }
}
