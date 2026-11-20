#pragma once
// The texture matches a release ships in wwmi_dx12\texture_matches (looks.texlook, lods.lodcache), never renamed.
// The cache takes each file once per version: its size and write time are kept in cache\texture_matches.taken, so a
// new release, a cleared cache or a missing cache file takes it again.
#include <mutex>
#include <string>

class ShippedTextureMatches {
public:
    // That file of the texture_matches folder beside `cache_dir` when the cache has not taken this version of it or
    // `own` (the cache's file it goes into) is missing; else empty.
    std::wstring ToTake(const std::wstring& cache_dir, const wchar_t* name, const std::wstring& own);
    bool MarkTaken(const std::wstring& cache_dir, const std::wstring& file);

private:
    static std::string Stamp(const std::wstring& file); // "<name> <size> <write time>"
    std::mutex mtx_;
};
inline ShippedTextureMatches g_shipped_matches;
