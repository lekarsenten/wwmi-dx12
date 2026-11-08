#include "CommandList.h"
#include "input.h"
#include "port_keygate.h"

void PortOptimiseCommandLists(HackerDevice* device);
void PortRegisterKeyBinding(const wchar_t* section, LPCWSTR ini_key, const wchar_t* key_name,
    shared_ptr<InputListener> listener, int auto_repeat, int down_delay, int up_delay,
    InputDisableScope input_disable_scope = InputDisableScope::INVALID);

#define optimise_command_lists PortOptimiseCommandLists
#define RegisterKeyBinding(...) PortRegisterKeyBinding(id, __VA_ARGS__)

#include "IniHandler.cpp"

#undef optimise_command_lists
#undef RegisterKeyBinding

void PortOptimiseCommandLists(HackerDevice* device)
{
    MigotoPort::WrapObservedCommands(registered_command_lists);
    optimise_command_lists(device);
}

void PortRegisterKeyBinding(const wchar_t* section, LPCWSTR ini_key, const wchar_t* key_name,
    shared_ptr<InputListener> listener, int auto_repeat, int down_delay, int up_delay,
    InputDisableScope input_disable_scope)
{
    if (listener && section && !wcscmp(ini_key, L"Key")) {
        wstring ns;
        get_section_namespace(section, &ns);
        listener = MigotoPort::GateKeyListener(listener, ns, section);
    }
    RegisterKeyBinding(ini_key, key_name, listener, auto_repeat, down_delay, up_delay, input_disable_scope);
}

bool PortSectionFolder(const wchar_t* section, wstring* ret)
{
    wchar_t base[MAX_PATH];
    wstring rel;
    if (!get_namespaced_section_path(section, &rel))
        return false;
    MigotoPort::MigotoDir(base, MAX_PATH);
    *ret = wstring(base) + rel;
    while (!ret->empty() && (ret->back() == L'\\' || ret->back() == L'/'))
        ret->pop_back();
    return !ret->empty();
}

bool PortIniNamespace(const wchar_t* abs_ini_path, wstring* ret)
{
    wchar_t base[MAX_PATH];
    MigotoPort::MigotoDir(base, MAX_PATH);
    wstring path(abs_ini_path);
    wstring root(base);
    for (auto& ch : path)
        if (ch == L'/') ch = L'\\';
    for (auto& ch : root)
        if (ch == L'/') ch = L'\\';
    if (root.empty() || path.size() <= root.size() ||
        _wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0)
        return false;
    const wstring rel = path.substr(root.size());

    for (auto& kv : ini_sections) {
        const IniSection& s = kv.second;
        const wstring&    file = s.ini_path.empty() ? s.ini_namespace : s.ini_path;
        if (_wcsicmp(file.c_str(), rel.c_str()) != 0)
            continue;
        *ret = s.ini_namespace;
        return !ret->empty();
    }
    return false;
}

void PortForEachIniFile(void (*cb)(const wstring& rel_path, const wstring& ns, void* user), void* user)
{
    std::set<wstring, WStringInsensitiveLess> seen;
    for (auto& kv : ini_sections) {
        const IniSection& s = kv.second;
        const wstring&    file = s.ini_path.empty() ? s.ini_namespace : s.ini_path;
        if (file.empty() || !seen.insert(file).second)
            continue;
        cb(file, s.ini_namespace, user);
    }
}
