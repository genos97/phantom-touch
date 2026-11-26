#pragma once

// X2 / F3 Connection State Machine & Auto-Reconnect Backoff
enum ConnState {
    CONN_NO_TOOLS = 0,
    CONN_NO_DEVICE,
    CONN_UNAUTHORIZED,
    CONN_READY,
    CONN_CONNECTING,
    CONN_STREAMING,
    CONN_RECONNECTING,
    CONN_LOST,
    CONN_ERROR
};

enum ConnectProgressStep {
    CONN_STEP_PUSH_SERVER = 0,
    CONN_STEP_START_SERVER = 1,
    CONN_STEP_WAIT_VIDEO = 2
};

struct ConnStateInput {
    bool toolsReady = false;
    bool streaming = false;
    bool connecting = false;
    bool reconnecting = false;
    bool reconnectCancelled = false;
    int reconnectAttempt = 0;
    int maxReconnectAttempts = 5;
    bool hasError = false;
    int readyDeviceCount = 0;
    int unauthorizedDeviceCount = 0;
};

class ConnectionStateMachine {
public:
    static int backoffMs(int attempt);
    static ConnState evaluate(const ConnStateInput& in);
    static const char* stateTitle(ConnState st);
    static const char* stepLabel(ConnectProgressStep step);
};
