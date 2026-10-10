#pragma once

#include <string>

namespace hub::app {

/// One tray balloon.
struct TrayNotice {
    std::string title;
    std::string message;
};

} // namespace hub::app
