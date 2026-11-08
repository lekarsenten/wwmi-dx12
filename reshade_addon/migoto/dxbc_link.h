#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MigotoPort {

enum class PixelLink {
    kLinks, // D3D12 links the pair as it is
    kRewritten, // `out` links it as D3D11 does
    kImpossible, // it does not link, and no rewrite reads what D3D11 reads
};

// `ps` rewritten so that D3D12 links it to `upstream` (the vs, ds or gs before it) as D3D11 does, register by register:
// an input takes the names the upstream outputs carry at its register, and SV_Position moves to the register the
// upstream writes it at. `note` says what changed, or why nothing could.
PixelLink LinkPixelInputs(const uint8_t* upstream, size_t upstream_size, const uint8_t* ps, size_t ps_size,
    std::vector<uint8_t>& out, std::string& note);

} // namespace MigotoPort
