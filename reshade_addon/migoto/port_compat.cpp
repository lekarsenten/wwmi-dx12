#include "port_compat.h"
#include <mutex>
#include <string>

namespace MigotoPort {

namespace {
std::mutex& Mtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}
std::wstring& IniRootStore()
{
    static auto* s = new std::wstring();
    return *s;
}
} // namespace

void SetIniRoot(const wchar_t* dir)
{
    std::lock_guard<std::mutex> l(Mtx());
    if (!dir || !*dir) {
        IniRootStore().clear();
        return;
    }
    std::wstring d(dir);
    if (d.back() != L'\\' && d.back() != L'/') d += L'\\'; // their code appends the filename directly
    IniRootStore() = d;
}

const wchar_t* IniRoot()
{
    std::lock_guard<std::mutex> l(Mtx());
    return IniRootStore().empty() ? nullptr : IniRootStore().c_str();
}

void MigotoDir(wchar_t* out, unsigned long count)
{
    if (!out || !count) return;
    if (const wchar_t* root = IniRoot()) {
        wcsncpy_s(out, count, root, _TRUNCATE);
        return;
    }
    if (!PortRealGetModuleFileNameW(migoto_handle, out, count)) {
        out[0] = 0;
        return;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        slash[1] = 0;
    else
        out[0] = 0;
}

} // namespace MigotoPort
