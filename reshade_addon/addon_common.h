#pragma once
#ifndef IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#endif
#include <imgui.h>
#include <reshade.hpp>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <d3d12.h>
#include <dxgi1_4.h> // IDXGIAdapter3::QueryVideoMemoryInfo -- the LOCAL/NON_LOCAL budget split
#include <psapi.h> // GetProcessMemoryInfo (PrivateUsage) -- the CPU-heap half of the memory row
#include <wrl.h>
#include <string>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <atomic>
#include <set>
#include <regex>
#include <cstdio>
#include <share.h>
#include <sstream>
#include <memory>
#include <functional>
#include <thread>

using namespace reshade::api;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
