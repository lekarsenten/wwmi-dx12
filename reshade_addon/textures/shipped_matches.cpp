#include "shipped_matches.h"
#include <windows.h>
#include <share.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::wstring TakenPath(const std::wstring& cache_dir)
{
    return cache_dir + L"\\texture_matches.taken";
}

bool IsFile(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::string> ReadLines(const std::wstring& path)
{
    std::vector<std::string> lines;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return lines;
    char b[512];
    while (fgets(b, sizeof b, f)) {
        size_t n = strlen(b);
        while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
        if (n) lines.emplace_back(b);
    }
    fclose(f);
    return lines;
}

std::string NameOf(const std::string& stamp)
{
    return stamp.substr(0, stamp.find(' '));
}
} // namespace

std::string ShippedTextureMatches::Stamp(const std::wstring& file)
{
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &d)) return {};
    const std::wstring name = file.substr(file.find_last_of(L"\\/") + 1);
    char b[320];
    std::snprintf(b, sizeof b, "%ls %llu %llu", name.c_str(),
        ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow,
        ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime);
    return b;
}

std::wstring ShippedTextureMatches::ToTake(const std::wstring& cache_dir, const wchar_t* name, const std::wstring& own)
{
    const size_t cut = cache_dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return {};
    const std::wstring file = cache_dir.substr(0, cut) + L"\\texture_matches\\" + name;
    if (!IsFile(file)) return {};
    if (!IsFile(own)) return file;
    const std::string stamp = Stamp(file);
    std::lock_guard l(mtx_);
    for (const std::string& line : ReadLines(TakenPath(cache_dir)))
        if (line == stamp) return {};
    return file;
}

bool ShippedTextureMatches::MarkTaken(const std::wstring& cache_dir, const std::wstring& file)
{
    const std::string stamp = Stamp(file);
    if (stamp.empty()) return false;
    std::lock_guard l(mtx_);
    const std::wstring path = TakenPath(cache_dir);
    std::vector<std::string> keep;
    for (const std::string& line : ReadLines(path))
        if (NameOf(line) != NameOf(stamp)) keep.push_back(line);
    keep.push_back(stamp);
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYNO);
    if (!f) return false;
    bool ok = true;
    for (const std::string& line : keep) ok = ok && fprintf(f, "%s\r\n", line.c_str()) > 0;
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
