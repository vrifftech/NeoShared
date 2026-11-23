#pragma once

#include <functional>

class wxFrame;

namespace neoupdate {
using RestartDecision = std::function<void(bool)>;
using PrepareRestart = std::function<void(RestartDecision)>;

void install(wxFrame& frame, const char* application, const char* version,
             PrepareRestart prepareRestart = {});
}
