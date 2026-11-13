#pragma once
// The addon names what it creates, so a DRED report tells our allocations from the game's.
#include <d3d12.h>

inline void DevName(ID3D12Object* obj, const wchar_t* name)
{
    if (obj) obj->SetName(name);
}
