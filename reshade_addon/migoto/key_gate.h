#pragma once
// The hotkey gate: a mod's [Key*] listener fires only when the add-on's gate allows its namespace and section.
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class InputListener;

namespace MigotoPort {

class KeyGate {
public:
    void SetGate(bool (*fn)(const wchar_t* ns, const wchar_t* section));
    bool Allows(const wchar_t* ns, const wchar_t* section);
    std::shared_ptr<InputListener> Wrap(
        std::shared_ptr<InputListener> inner, const std::wstring& ns, const std::wstring& section);
    size_t WrappedCount() const;

private:
    std::mutex mutex_;
    bool (*gate_)(const wchar_t*, const wchar_t*) = nullptr;
    std::atomic<size_t> wrapped_{ 0 };
};
extern KeyGate& g_key_gate;

// Their IniHandler (port_ini.cpp) registers every [Key*] listener through this.
std::shared_ptr<InputListener> GateKeyListener(
    std::shared_ptr<InputListener> inner, const std::wstring& ns, const std::wstring& section);

} // namespace MigotoPort
