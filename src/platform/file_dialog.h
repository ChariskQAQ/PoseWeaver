#pragma once
#include <string>

// Native file dialogs. Paths are UTF-8; an empty string means cancelled.
// `filterSpec` is a ';'-separated pattern list such as "*.png;*.jpg".
std::string openFileDialog(const char* filterName, const char* filterSpec);
std::string saveFileDialog(const char* filterName, const char* filterSpec, const char* defaultExt);
