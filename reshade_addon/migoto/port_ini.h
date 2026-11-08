#pragma once
// What port_ini.cpp answers from their parsed ini sections: a section's folder, a file's namespace, every file.
#include <string>

bool PortSectionFolder(const wchar_t* section, std::wstring* ret);
bool PortIniNamespace(const wchar_t* abs_ini_path, std::wstring* ret);
void PortForEachIniFile(void (*cb)(const std::wstring& rel_path, const std::wstring& ns, void* user), void* user);
