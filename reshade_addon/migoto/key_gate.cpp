#include "key_gate.h"
#include "input.h"

namespace MigotoPort {

KeyGate& g_key_gate = *new KeyGate();

namespace {
class GatedKeyListener : public InputListener {
    std::shared_ptr<InputListener> inner_;
    std::wstring ns_;
    std::wstring section_;

public:
    GatedKeyListener(std::shared_ptr<InputListener> inner, const std::wstring& ns, const std::wstring& section)
        : inner_(std::move(inner)),
          ns_(ns),
          section_(section)
    {}
    void DownEvent(HackerDevice* device) override
    {
        if (!g_key_gate.Allows(ns_.c_str(), section_.c_str())) return;
        inner_->DownEvent(device);
    }
    void UpEvent(HackerDevice* device) override { inner_->UpEvent(device); }
};
} // namespace

void KeyGate::SetGate(bool (*fn)(const wchar_t* ns, const wchar_t* section))
{
    std::lock_guard<std::mutex> l(mutex_);
    gate_ = fn;
}

bool KeyGate::Allows(const wchar_t* ns, const wchar_t* section)
{
    bool (*gate)(const wchar_t*, const wchar_t*);
    {
        std::lock_guard<std::mutex> l(mutex_);
        gate = gate_;
    }
    return !gate || gate(ns, section);
}

std::shared_ptr<InputListener> KeyGate::Wrap(
    std::shared_ptr<InputListener> inner, const std::wstring& ns, const std::wstring& section)
{
    if (!inner) return inner;
    wrapped_.fetch_add(1, std::memory_order_relaxed);
    return std::make_shared<GatedKeyListener>(std::move(inner), ns, section);
}

size_t KeyGate::WrappedCount() const
{
    return wrapped_.load(std::memory_order_relaxed);
}

std::shared_ptr<InputListener> GateKeyListener(
    std::shared_ptr<InputListener> inner, const std::wstring& ns, const std::wstring& section)
{
    return g_key_gate.Wrap(std::move(inner), ns, section);
}

} // namespace MigotoPort
