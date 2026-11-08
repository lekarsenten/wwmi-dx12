#pragma once
// Debug builds name what the addon creates, so a DRED report tells our allocations from the game's.
#include <d3d12.h>

inline void DevName(ID3D12Object* obj, const wchar_t* name)
{
#ifdef WWMI_DEBUG
    if (obj) obj->SetName(name);
#else
    (void)obj;
    (void)name;
#endif
}
