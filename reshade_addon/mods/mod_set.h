#pragma once
#include <memory>
#include <vector>
#include "component_table.h"
#include "mod_registry.h"
#include "../core/set_life.h"
#include "../migoto/port_api.h"

class ModSet;

struct SetState {
    virtual ~SetState() = default;
    virtual void Build(ModSet&) {}
};

unsigned RegisterSetState(SetState* (*make)());

template <class T> struct SetStateSlot {
    static SetState* Make() { return new T; }
    static inline const unsigned index = RegisterSetState(&Make);
};

class ModSet : public SetLifeRefs {
public:
    ModSet();
    ModSet(const ModSet&) = delete;
    ModSet& operator=(const ModSet&) = delete;
    ~ModSet() override;

    template <class T> T& State() { return *static_cast<T*>(states_[SetStateSlot<T>::index].get()); }
    void BuildStates();

    unsigned id = 0;
    unsigned retired_frame = 0;
    ModRegistry mods;
    ComponentTable comps;
    std::vector<PortApi::HeldBuffer> held;

private:
    std::vector<std::unique_ptr<SetState>> states_;
};

extern SetLife<ModSet> g_mod_sets;

void InitModSets();
void BuildModSet(void* user);
void DrainModSets();

class ModSetScope {
public:
    explicit ModSetScope(const void* cmd_list);
    ~ModSetScope();
    ModSetScope(const ModSetScope&) = delete;
    ModSetScope& operator=(const ModSetScope&) = delete;

private:
    ModSet* prev_;
    bool hazard_;
};

class ModSetStepScope {
public:
    ModSetStepScope();
    ~ModSetStepScope();
    ModSetStepScope(const ModSetStepScope&) = delete;
    ModSetStepScope& operator=(const ModSetStepScope&) = delete;

private:
    ModSet* prev_;
    bool prev_step_;
};

class ModSetRef {
public:
    ModSetRef();
    ~ModSetRef();
    ModSetRef(const ModSetRef&) = delete;
    ModSetRef& operator=(const ModSetRef&) = delete;
    ModSet& operator*() const { return *set_; }

private:
    ModSet* set_;
    ModSet* prev_;
};

ModSet& CurrentSet();
ModRegistry& Mods();
ComponentTable& Comps();

inline uint64_t ModNsHash(int mod_id)
{
    ModRegistry& mods = Mods();
    return (mod_id >= 0 && (size_t)mod_id < mods.Count()) ? mods.At(mod_id).ns_hash : 0;
}
