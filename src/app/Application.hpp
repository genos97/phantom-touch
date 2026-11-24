#pragma once

#include <string>

// Top-level application bootstrapper: CLI parsing, DPI awareness,
// profile resolution, ScrcpyBridge connection, InputEngine & OverlayWindow lifecycle.
class Application {
public:
    int run(int argc, char** argv);

private:
    static std::string getExeDir();
    static void enableDpiAwareness();
    static void printUsage();
};
