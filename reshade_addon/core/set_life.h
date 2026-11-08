#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct SetLifeRefs {
    std::atomic<long> refs{ 1 };
    std::atomic<unsigned> pinned_by{ 0 };
    virtual ~SetLifeRefs() = default;
};

template <class T> class SetLife {
public:
    struct Hazard {
        std::atomic<T*> set{ nullptr };
    };

    T* Current() const { return current_.load(std::memory_order_acquire); }

    T* AcquireCurrent() const
    {
        std::lock_guard<std::mutex> l(mtx_);
        T* t = current_.load(std::memory_order_acquire);
        if (t) AddRef(t);
        return t;
    }

    T* Publish(T* next, uint64_t retire_stamp)
    {
        T* old = current_.exchange(next, std::memory_order_seq_cst);
        if (old) Retire(old, retire_stamp);
        return old;
    }

    void Retire(T* t, uint64_t stamp)
    {
        std::lock_guard<std::mutex> l(mtx_);
        retired_.push_back({ t, stamp, false });
    }

    Hazard* RegisterHazard()
    {
        Hazard* h = new Hazard;
        std::lock_guard<std::mutex> l(mtx_);
        hazards_.push_back(h);
        return h;
    }

    void UnregisterHazard(Hazard* h)
    {
        {
            std::lock_guard<std::mutex> l(mtx_);
            for (size_t i = 0; i < hazards_.size(); ++i)
                if (hazards_[i] == h) {
                    hazards_[i] = hazards_.back();
                    hazards_.pop_back();
                    break;
                }
        }
        delete h;
    }

    T* Protect(Hazard& h) const
    {
        T* cur = current_.load(std::memory_order_seq_cst);
        for (;;) {
            h.set.store(cur, std::memory_order_seq_cst);
            T* const now = current_.load(std::memory_order_seq_cst);
            if (now == cur) return cur;
            cur = now;
        }
    }

    static void Unprotect(Hazard& h) { h.set.store(nullptr, std::memory_order_release); }

    size_t Drain(bool (*passed)(uint64_t stamp))
    {
        std::vector<T*> dead;
        {
            std::lock_guard<std::mutex> l(mtx_);
            size_t keep = 0;
            for (Retired& r : retired_) {
                if (!r.unref) {
                    if (!passed(r.stamp)) {
                        retired_[keep++] = r;
                        continue;
                    }
                    if (r.t->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) continue;
                    r.unref = true;
                }
                if (Hazarded(r.t))
                    retired_[keep++] = r;
                else
                    dead.push_back(r.t);
            }
            retired_.resize(keep);
        }
        for (T* t : dead) delete t;
        return dead.size();
    }

    static void AddRef(T* t) { t->refs.fetch_add(1, std::memory_order_relaxed); }

private:
    struct Retired {
        T* t;
        uint64_t stamp;
        bool unref;
    };
    bool Hazarded(const T* t) const
    {
        for (const Hazard* h : hazards_)
            if (h->set.load(std::memory_order_seq_cst) == t) return true;
        return false;
    }
    std::atomic<T*> current_{ nullptr };
    mutable std::mutex mtx_;
    std::vector<Retired> retired_;
    std::vector<Hazard*> hazards_;
};

template <class T> class ListPins {
public:
    T* Pin(const SetLife<T>& life)
    {
        T* cur = life.Current();
        if (cur == last_ || !cur) return cur;
        cur = life.AcquireCurrent();
        cur->pinned_by.fetch_add(1, std::memory_order_relaxed);
        held_.push_back(cur);
        last_ = cur;
        return cur;
    }

    void Executed(SetLife<T>& life, uint64_t stamp)
    {
        for (T* t : held_) {
            SetLife<T>::AddRef(t);
            life.Retire(t, stamp);
        }
    }

    void Reset(SetLife<T>& life)
    {
        for (T* t : held_) life.Retire(t, 0);
        held_.clear();
        last_ = nullptr;
    }

    size_t Held() const { return held_.size(); }

private:
    std::vector<T*> held_;
    T* last_ = nullptr;
};
