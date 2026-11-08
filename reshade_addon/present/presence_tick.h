#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The hash-trigger sections no draw fires, run once per frame per on-screen mod; rebuilt on each parse.
class PresenceTriggers {
public:
    void Tick();

private:
    struct PerSet;
};
extern PresenceTriggers g_presence;
