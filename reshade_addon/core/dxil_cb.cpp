// ===========================================================================
//  dxil_cb.cpp -- see dxil_cb.h. A minimal LLVM 3.7 bitstream reader: enough of the module to resolve
//  the `!dx.resources` CBV records -- the type table (struct names), the module-level constants (the
//  i32 fields), the metadata block (strings, values, nodes, the named node).
// ===========================================================================
#include "dxil_cb.h"
#include <cstring>
#include <map>

namespace {

// ---- the container -----------------------------------------------------------------------------
uint32_t rd32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

const uint8_t* find_bitcode(const uint8_t* p, size_t n, size_t& bc_len, std::string* why)
{
    auto is_bc = [](const uint8_t* q, size_t left) {
        return left >= 4 && q[0] == 'B' && q[1] == 'C' && q[2] == 0xC0 && q[3] == 0xDE;
    };
    if (is_bc(p, n)) {
        bc_len = n;
        return p;
    }
    // a DXIL part: DxilProgramHeader { ProgramVersion, SizeInUint32, { 'DXIL', DxilVersion, BitcodeOffset, BitcodeSize } }
    auto from_part = [&](const uint8_t* part, size_t plen) -> const uint8_t* {
        if (plen < 24 || memcmp(part + 8, "DXIL", 4) != 0) return nullptr;
        const uint32_t off = rd32(part + 16);
        const uint32_t sz = rd32(part + 20);
        const uint8_t* bc = part + 8 + off; // offset is from the bitcode header ('DXIL')
        if ((size_t)(bc - part) + sz > plen || !is_bc(bc, sz)) return nullptr;
        bc_len = sz;
        return bc;
    };
    if (n >= 24 && memcmp(p + 8, "DXIL", 4) == 0) {
        if (const uint8_t* bc = from_part(p, n)) return bc;
    }
    if (n >= 32 && memcmp(p, "DXBC", 4) == 0) {
        const uint32_t total = rd32(p + 24);
        const uint32_t parts = rd32(p + 28);
        if (total > n) {
            if (why) *why = "container size past the buffer";
            return nullptr;
        }
        for (uint32_t i = 0; i < parts && 32 + 4 * i + 4 <= n; ++i) {
            const uint64_t po = rd32(p + 32 + 4 * i);
            if (po + 8 > n) break;
            const uint64_t psz = rd32(p + po + 4);
            if (po + 8 + psz > n) break;
            if (memcmp(p + po, "DXIL", 4) == 0) {
                if (const uint8_t* bc = from_part(p + po + 8, psz)) return bc;
            }
        }
        if (why) *why = "no DXIL part with bitcode";
        return nullptr;
    }
    if (why) *why = "not a DXBC container, DXIL part or bitcode";
    return nullptr;
}

// ---- the bitstream -----------------------------------------------------------------------------
struct AbbrevOp {
    enum Kind : uint8_t { Literal, Fixed, VBR, Array, Char6, Blob } kind;
    uint64_t val;
};
struct Abbrev {
    std::vector<AbbrevOp> ops;
};

struct Record {
    uint32_t code = 0;
    std::vector<uint64_t> ops;
    std::vector<uint8_t> blob;
};
constexpr uint64_t kMaxRecordOps = 1u << 20;
constexpr uint64_t kMaxBlobBytes = 1u << 24;

class Bits {
public:
    Bits(const uint8_t* p, size_t n)
        : p_(p),
          nbits_(n * 8)
    {}
    bool eof() const { return pos_ >= nbits_; }
    bool bad() const { return bad_; }
    uint64_t read(unsigned w)
    { // w <= 64
        uint64_t v = 0;
        unsigned got = 0;
        while (got < w) {
            if (pos_ >= nbits_) {
                bad_ = true;
                return 0;
            }
            const unsigned byte = (unsigned)(pos_ >> 3);
            const unsigned bit = (unsigned)(pos_ & 7);
            const unsigned take = (std::min)(8u - bit, w - got);
            v |= (uint64_t)((p_[byte] >> bit) & ((1u << take) - 1u)) << got;
            got += take;
            pos_ += take;
        }
        return v;
    }
    uint64_t vbr(unsigned w)
    {
        uint64_t v = 0;
        unsigned shift = 0;
        const uint64_t hi = 1ull << (w - 1);
        for (;;) {
            const uint64_t piece = read(w);
            if (bad_) return 0;
            v |= (piece & (hi - 1)) << shift;
            if (!(piece & hi)) return v;
            shift += w - 1;
            if (shift > 63) {
                bad_ = true;
                return 0;
            }
        }
    }
    void align32()
    {
        pos_ = (pos_ + 31) & ~(uint64_t)31;
        if (pos_ > nbits_) bad_ = true;
    }
    void skip_words(uint64_t words)
    {
        pos_ += words * 32;
        if (pos_ > nbits_) bad_ = true;
    }

private:
    const uint8_t* p_;
    uint64_t nbits_;
    uint64_t pos_ = 0;
    bool bad_ = false;
};

char char6(uint64_t v)
{
    if (v < 26) return (char)('a' + v);
    if (v < 52) return (char)('A' + v - 26);
    if (v < 62) return (char)('0' + v - 52);
    return v == 62 ? '.' : '_';
}

// A block being read: its abbrev id width and abbrev list (blockinfo's for this id, then local ones).
struct Block {
    uint32_t id;
    unsigned width;
    std::vector<Abbrev> abbrevs;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n)
        : b_(p, n)
    {}
    Bits& bits() { return b_; }
    std::map<uint32_t, std::vector<Abbrev>> blockinfo; // block id -> abbrevs registered in BLOCKINFO

    // Reads one abbrev definition (after the DEFINE_ABBREV id).
    bool read_abbrev_def(Abbrev& a)
    {
        const uint64_t nops = b_.vbr(5);
        for (uint64_t i = 0; i < nops; ++i) {
            AbbrevOp op{};
            if (b_.read(1)) {
                op.kind = AbbrevOp::Literal;
                op.val = b_.vbr(8);
            } else {
                const uint64_t enc = b_.read(3);
                switch (enc) {
                case 1:
                    op.kind = AbbrevOp::Fixed;
                    op.val = b_.vbr(5);
                    break;
                case 2:
                    op.kind = AbbrevOp::VBR;
                    op.val = b_.vbr(5);
                    break;
                case 3: op.kind = AbbrevOp::Array; break;
                case 4: op.kind = AbbrevOp::Char6; break;
                case 5: op.kind = AbbrevOp::Blob; break;
                default: return false;
                }
            }
            if (b_.bad()) return false;
            a.ops.push_back(op);
        }
        return !b_.bad();
    }
    uint64_t read_scalar(const AbbrevOp& op)
    {
        switch (op.kind) {
        case AbbrevOp::Literal: return op.val;
        case AbbrevOp::Fixed: return op.val ? b_.read((unsigned)op.val) : 0;
        case AbbrevOp::VBR: return op.val ? b_.vbr((unsigned)op.val) : 0;
        case AbbrevOp::Char6: return (uint64_t)(uint8_t)char6(b_.read(6));
        default: return 0;
        }
    }
    // Reads a record body given its abbrev id (already consumed). The record code may be a literal.
    bool read_record(const Block& blk, uint64_t abbrev_id, Record& r)
    {
        r.ops.clear();
        r.blob.clear();
        if (abbrev_id == 3) { // UNABBREV_RECORD
            r.code = (uint32_t)b_.vbr(6);
            const uint64_t n = b_.vbr(6);
            if (n > kMaxRecordOps) return false;
            r.ops.reserve((size_t)n);
            for (uint64_t i = 0; i < n; ++i) r.ops.push_back(b_.vbr(6));
            return !b_.bad();
        }
        if (abbrev_id < 4 || abbrev_id - 4 >= blk.abbrevs.size()) return false;
        const Abbrev& a = blk.abbrevs[(size_t)abbrev_id - 4];
        bool first = true;
        for (size_t i = 0; i < a.ops.size(); ++i) {
            const AbbrevOp& op = a.ops[i];
            if (op.kind == AbbrevOp::Array) {
                if (i + 1 >= a.ops.size()) return false;
                const AbbrevOp& el = a.ops[i + 1];
                const uint64_t n = b_.vbr(6);
                if (n > kMaxRecordOps) return false;
                for (uint64_t k = 0; k < n; ++k) {
                    const uint64_t v = read_scalar(el);
                    if (first) {
                        r.code = (uint32_t)v;
                        first = false;
                    } else
                        r.ops.push_back(v);
                }
                ++i; // the element op is consumed with the array
                continue;
            }
            if (op.kind == AbbrevOp::Blob) {
                const uint64_t n = b_.vbr(6);
                b_.align32();
                if (n > kMaxBlobBytes) return false;
                r.blob.resize((size_t)n);
                for (uint64_t k = 0; k < n; ++k) r.blob[(size_t)k] = (uint8_t)b_.read(8);
                b_.align32();
                if (first) {
                    r.code = 0;
                    first = false;
                }
                continue;
            }
            const uint64_t v = read_scalar(op);
            if (first) {
                r.code = (uint32_t)v;
                first = false;
            } else
                r.ops.push_back(v);
        }
        return !b_.bad();
    }
    // Enters a subblock (after the ENTER_SUBBLOCK id): fills `blk`, leaves the cursor at its first item.
    bool enter(Block& blk, uint64_t& body_words)
    {
        blk.id = (uint32_t)b_.vbr(8);
        blk.width = (unsigned)b_.vbr(4);
        b_.align32();
        body_words = b_.read(32);
        if (b_.bad() || !blk.width || blk.width > 32) return false;
        auto bi = blockinfo.find(blk.id);
        blk.abbrevs = bi == blockinfo.end() ? std::vector<Abbrev>() : bi->second;
        return true;
    }
    // Skips a block whose header `enter` just read.
    void skip(uint64_t body_words) { b_.skip_words(body_words); }
    // Reads the BLOCKINFO block (the cursor is at its first item).
    bool read_blockinfo(Block& blk)
    {
        uint32_t cur = ~0u;
        for (;;) {
            const uint64_t id = b_.read(blk.width);
            if (b_.bad()) return false;
            if (id == 0) {
                b_.align32();
                return !b_.bad();
            } // END_BLOCK
            if (id == 1) {
                Block sub;
                uint64_t w;
                if (!enter(sub, w)) return false;
                skip(w);
                continue;
            }
            if (id == 2) {
                Abbrev a;
                if (!read_abbrev_def(a)) return false;
                if (cur != ~0u) blockinfo[cur].push_back(a);
                continue;
            }
            Record r;
            if (!read_record(blk, id, r)) return false;
            if (r.code == 1 && !r.ops.empty()) cur = (uint32_t)r.ops[0]; // SETBID
        }
    }

private:
    Bits b_;
};

// ---- the module ----------------------------------------------------------------------------------
enum : uint32_t { BLOCKINFO_ID = 0, MODULE_ID = 8, CONSTANTS_ID = 11, METADATA_ID = 15, TYPE_NEW_ID = 17 };

struct TypeInfo {
    bool is_ptr = false;
    bool is_int = false;
    uint32_t pointee = ~0u;
    std::string name;
};
struct ValueInfo {
    uint32_t type = ~0u;
    bool is_int = false;
    int64_t ival = 0;
};
struct MdNode {
    enum Kind : uint8_t { None, String, Value, Node } kind = None;
    std::string str;
    uint32_t value = ~0u;
    std::vector<int64_t> elems; // elems: md id, -1 = null
};

struct Module {
    std::vector<TypeInfo> types;
    std::vector<ValueInfo> values; // globals (type only), then module-level constants
    std::vector<MdNode> md;
    std::map<std::string, std::vector<uint64_t>> named; // named metadata -> md ids
};

// Iterates a block's items; `on_record` gets each record, `on_block` each subblock (return false to skip).
template <class OnRecord, class OnBlock> bool walk(Reader& R, Block& blk, OnRecord on_record, OnBlock on_block)
{
    for (;;) {
        const uint64_t id = R.bits().read(blk.width);
        if (R.bits().bad()) return false;
        if (id == 0) {
            R.bits().align32();
            return !R.bits().bad();
        }
        if (id == 1) {
            Block sub;
            uint64_t w;
            if (!R.enter(sub, w)) return false;
            if (sub.id == BLOCKINFO_ID) {
                if (!R.read_blockinfo(sub)) return false;
                continue;
            }
            if (!on_block(sub)) R.skip(w);
            if (R.bits().bad()) return false;
            continue;
        }
        if (id == 2) {
            Abbrev a;
            if (!R.read_abbrev_def(a)) return false;
            blk.abbrevs.push_back(a);
            continue;
        }
        Record r;
        if (!R.read_record(blk, id, r)) return false;
        if (!on_record(r)) return false;
    }
}

std::string chars_of(const std::vector<uint64_t>& ops, size_t from = 0)
{
    std::string s;
    s.reserve(ops.size());
    for (size_t i = from; i < ops.size(); ++i) s.push_back((char)(uint8_t)ops[i]);
    return s;
}

bool read_types(Reader& R, Block& blk, Module& M)
{
    std::string pending_name;
    return walk(
        R, blk,
        [&](const Record& r) {
            TypeInfo t;
            switch (r.code) {
            case 1: return true; // NUMENTRY
            case 19: pending_name = chars_of(r.ops); return true; // STRUCT_NAME
            case 7: t.is_int = true; break; // INTEGER [width]
            case 8:
                t.is_ptr = true;
                if (!r.ops.empty()) t.pointee = (uint32_t)r.ops[0];
                break; // POINTER
            case 6:
            case 20:
                t.name = pending_name;
                pending_name.clear();
                break; // OPAQUE / STRUCT_NAMED
            default: break;
            }
            M.types.push_back(t);
            return true;
        },
        [&](Block&) { return false; });
}

bool read_constants(Reader& R, Block& blk, Module& M)
{
    uint32_t cur_type = ~0u;
    return walk(
        R, blk,
        [&](const Record& r) {
            if (r.code == 1) {
                cur_type = r.ops.empty() ? ~0u : (uint32_t)r.ops[0];
                return true;
            } // SETTYPE
            ValueInfo v;
            v.type = cur_type;
            if (r.code == 4 && !r.ops.empty()) { // INTEGER, signed VBR
                const uint64_t u = r.ops[0];
                v.is_int = true;
                v.ival = (u & 1) ? -(int64_t)(u >> 1) : (int64_t)(u >> 1);
            } else if (r.code == 2 && cur_type < M.types.size() && M.types[cur_type].is_int) { // NULL of an integer
                v.is_int = true;
                v.ival = 0; // type: `i32 0`
            }
            M.values.push_back(v);
            return true;
        },
        [&](Block&) { return false; });
}

bool read_metadata(Reader& R, Block& blk, Module& M)
{
    std::string pending_name;
    return walk(
        R, blk,
        [&](const Record& r) {
            MdNode n;
            switch (r.code) {
            case 4: pending_name = chars_of(r.ops); return true; // NAME
            case 6:
            case 11: return true; // KIND, ATTACHMENT
            case 10: { // NAMED_NODE
                std::vector<uint64_t> ids(r.ops.begin(), r.ops.end());
                M.named[pending_name] = ids;
                pending_name.clear();
                return true;
            }
            case 1:
                n.kind = MdNode::String;
                n.str = chars_of(r.ops);
                break; // STRING
            case 2:
                n.kind = MdNode::Value;
                if (r.ops.size() >= 2) n.value = (uint32_t)r.ops[1];
                break; // VALUE [ty, val]
            case 3:
            case 5: // NODE / DISTINCT_NODE: n x (md+1)
                n.kind = MdNode::Node;
                n.elems.reserve(r.ops.size());
                for (uint64_t e : r.ops) n.elems.push_back(e ? (int64_t)e - 1 : -1);
                break;
            default: break; // debug-info nodes: a slot each
            }
            M.md.push_back(n);
            return true;
        },
        [&](Block&) { return false; });
}

bool read_module(Reader& R, Block& blk, Module& M)
{
    return walk(
        R, blk,
        [&](const Record& r) {
            if (r.code == 7 || r.code == 8 || r.code == 9 || r.code == 14) { // GLOBALVAR / FUNCTION / ALIAS (old, new)
                ValueInfo v;
                v.type = r.ops.empty() ? ~0u : (uint32_t)r.ops[0];
                M.values.push_back(v);
            }
            return true;
        },
        [&](Block& sub) {
            if (sub.id == TYPE_NEW_ID) return read_types(R, sub, M);
            if (sub.id == CONSTANTS_ID) return read_constants(R, sub, M);
            if (sub.id == METADATA_ID) return read_metadata(R, sub, M);
            return false; // functions, symtab, ...: skipped
        });
}

// ---- the CBV records -----------------------------------------------------------------------------
const MdNode* node(const Module& M, int64_t id)
{
    return id >= 0 && (size_t)id < M.md.size() ? &M.md[(size_t)id] : nullptr;
}

bool md_int(const Module& M, int64_t id, uint32_t& out)
{
    const MdNode* n = node(M, id);
    if (!n || n->kind != MdNode::Value || n->value >= M.values.size() || !M.values[n->value].is_int) return false;
    out = (uint32_t)M.values[n->value].ival;
    return true;
}
std::string md_type_name(const Module& M, int64_t id)
{
    const MdNode* n = node(M, id);
    if (!n || n->kind != MdNode::Value || n->value >= M.values.size()) return "";
    uint32_t t = M.values[n->value].type;
    if (t < M.types.size() && M.types[t].is_ptr) t = M.types[t].pointee;
    return t < M.types.size() ? M.types[t].name : "";
}

} // namespace

bool DxilCbuffers(const void* bytes, size_t len, std::vector<DxilCbuf>& out, std::string* why)
{
    out.clear();
    const uint8_t* p = static_cast<const uint8_t*>(bytes);
    size_t bc_len = 0;
    const uint8_t* bc = find_bitcode(p, len, bc_len, why);
    if (!bc) return false;
    Reader R(bc + 4, bc_len - 4); // past the 'BC' 0xC0DE magic
    Module M;
    bool got_module = false;
    // top level: abbrev width 2; blocks only
    for (;;) {
        if (R.bits().eof()) break;
        const uint64_t id = R.bits().read(2);
        if (R.bits().bad()) break;
        if (id != 1) {
            if (why) *why = "unexpected top-level item";
            return false;
        }
        Block blk;
        uint64_t w;
        if (!R.enter(blk, w)) {
            if (why) *why = "bad block header";
            return false;
        }
        if (blk.id == MODULE_ID) {
            if (!read_module(R, blk, M)) {
                if (why) *why = "module block did not parse";
                return false;
            }
            got_module = true;
            break;
        }
        if (blk.id == BLOCKINFO_ID) {
            if (!R.read_blockinfo(blk)) {
                if (why) *why = "blockinfo did not parse";
                return false;
            }
            continue;
        }
        R.skip(w); // IDENTIFICATION etc.
        if (R.bits().bad()) break;
    }
    if (!got_module) {
        if (why) *why = "no module block";
        return false;
    }
    auto nm = M.named.find("dx.resources");
    if (nm == M.named.end()) return true; // no resources at all
    if (nm->second.empty()) {
        if (why) *why = "dx.resources names no node";
        return false;
    }
    const MdNode* res = node(M, (int64_t)nm->second[0]);
    if (!res || res->kind != MdNode::Node || res->elems.size() < 3) {
        if (why) *why = "dx.resources is not a 4-list";
        return false;
    }
    const MdNode* cbvs = node(M, res->elems[2]); // {srvs, uavs, CBVS, samplers}
    if (!cbvs) return true; // null = no constant buffers
    if (cbvs->kind != MdNode::Node) {
        if (why) *why = "the CBV list is not a node";
        return false;
    }
    for (int64_t e : cbvs->elems) {
        const MdNode* rec = node(M, e);
        if (!rec || rec->kind != MdNode::Node || rec->elems.size() < 7) {
            if (why) *why = "a CBV record is short";
            return false;
        }
        // {i32 id, %type* undef, !"name", i32 space, i32 lower bound, i32 range size, i32 size in bytes, extended}
        DxilCbuf c;
        const MdNode* name = node(M, rec->elems[2]);
        if (name && name->kind == MdNode::String) c.name = name->str;
        c.type = md_type_name(M, rec->elems[1]);
        if (!md_int(M, rec->elems[3], c.space) || !md_int(M, rec->elems[4], c.reg) ||
            !md_int(M, rec->elems[5], c.count) || !md_int(M, rec->elems[6], c.bytes)) {
            if (why) *why = "a CBV record's fields are not i32 constants";
            return false;
        }
        out.push_back(c);
    }
    return true;
}
