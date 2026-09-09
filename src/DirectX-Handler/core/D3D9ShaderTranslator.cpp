// Shader Model 1-3 bytecode to HLSL.
//
// Most Wanted ships its shaders already compiled, as D3D9 token streams. There
// is no source to recompile and no way to hand those tokens to D3D11 or D3D12,
// so this file disassembles them and re-emits equivalent HLSL, which
// D3DCompile then builds for the target backend. Both backends use this; it is
// the single largest piece of shared code in the project.
//
// The pipeline is Parser -> Emitter -> HLSL string:
//
//   Parser   walks the token stream into a flat instruction list. The format
//            is self-describing: a version token, then instructions whose
//            length is carried in the token itself, terminated by 0x0000FFFF.
//   Emitter  walks that list and writes HLSL, mapping D3D9's register file
//            onto arrays and its instruction set onto HLSL expressions.
//
// The register model is reproduced rather than abstracted away. D3D9 shaders
// address a flat constant file (c0-c255 float, i0-i15 integer, b0-b15 bool)
// and numbered temporaries, and the game sets constants by register index
// through SetVertexShaderConstantF and friends. Emitting named variables would
// mean tracking which name each index maps to, so instead the constant file
// becomes literal arrays — c[], ic[], bc[] — indexed exactly as the bytecode
// indexes them. ConstantMapper then uploads the game's writes straight into
// those arrays with no translation at all. Temporaries become r0, r1, ... for
// the same reason.
//
// Three things D3D9 handled outside the shader have to be folded into it here,
// because D3D11 and D3D12 have no fixed-function equivalent:
//
//   Alpha test  a render state in D3D9; emitted as a trailing discard,
//               selected by the DX9_ATEST macro.
//   Fog         a render state in D3D9; emitted as a trailing blend toward the
//               fog colour, selected by DX9_FOG.
//   Sampler kind D3D9 bytecode does not always say whether a sampler is 2D,
//               cube or volume. DX9_TEXKINDn carries what the runtime observed
//               was bound.
//
// All three are #defines rather than baked-in code so that one translation can
// serve many render states: ShaderCache compiles the same HLSL repeatedly with
// different macro values and caches the results as variants. That is why the
// emitted text always contains every alpha-test comparison, not just the one
// in use.
//
// Anything unsupported must fail loudly and return false. A shader that
// silently translates to something almost-right is far more expensive to
// diagnose than one that refuses.

#include <core/D3D9ShaderTranslator.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mwon12 {
namespace {

enum Op : uint16_t {
    OP_NOP = 0,   OP_MOV = 1,   OP_ADD = 2,   OP_SUB = 3,   OP_MAD = 4,
    OP_MUL = 5,   OP_RCP = 6,   OP_RSQ = 7,   OP_DP3 = 8,   OP_DP4 = 9,
    OP_MIN = 10,  OP_MAX = 11,  OP_SLT = 12,  OP_SGE = 13,  OP_EXP = 14,
    OP_LOG = 15,  OP_LIT = 16,  OP_DST = 17,  OP_LRP = 18,  OP_FRC = 19,
    OP_M4x4 = 20, OP_M4x3 = 21, OP_M3x4 = 22, OP_M3x3 = 23, OP_M3x2 = 24,
    OP_CALL = 25, OP_CALLNZ = 26, OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29,
    OP_LABEL = 30, OP_DCL = 31, OP_POW = 32, OP_CRS = 33, OP_SGN = 34,
    OP_ABS = 35,  OP_NRM = 36,  OP_SINCOS = 37, OP_REP = 38, OP_ENDREP = 39,
    OP_IF = 40,   OP_IFC = 41,  OP_ELSE = 42, OP_ENDIF = 43, OP_BREAK = 44,
    OP_BREAKC = 45, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48,

    OP_TEXCOORD = 64,  OP_TEXKILL = 65,  OP_TEX = 66,       OP_TEXBEM = 67,
    OP_TEXBEML = 68,   OP_TEXREG2AR = 69, OP_TEXREG2GB = 70,
    OP_TEXM3x2PAD = 71, OP_TEXM3x2TEX = 72, OP_TEXM3x3PAD = 73,
    OP_TEXM3x3TEX = 74, OP_TEXM3x3DIFF = 75, OP_TEXM3x3SPEC = 76,
    OP_TEXM3x3VSPEC = 77, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80,
    OP_DEF = 81,  OP_TEXREG2RGB = 82, OP_TEXDP3TEX = 83, OP_TEXM3x2DEPTH = 84,
    OP_TEXDP3 = 85, OP_TEXM3x3 = 86, OP_TEXDEPTH = 87, OP_CMP = 88,
    OP_BEM = 89,  OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93,
    OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96,

    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};

enum RegFile : uint8_t {
    RF_TEMP = 0, RF_INPUT = 1, RF_CONST = 2, RF_ADDR = 3  ,
    RF_RASTOUT = 4, RF_ATTROUT = 5, RF_TEXCRDOUT = 6  ,
    RF_CONSTINT = 7, RF_COLOROUT = 8, RF_DEPTHOUT = 9, RF_SAMPLER = 10,
    RF_CONST2 = 11, RF_CONST3 = 12, RF_CONST4 = 13, RF_CONSTBOOL = 14,
    RF_LOOP = 15, RF_TEMPFLOAT16 = 16, RF_MISC = 17, RF_LABEL = 18,
    RF_PREDICATE = 19,
};

enum SrcMod : uint8_t {
    SM_NONE = 0, SM_NEG = 1, SM_BIAS = 2, SM_BIASNEG = 3, SM_SIGN = 4,
    SM_SIGNNEG = 5, SM_COMP = 6, SM_X2 = 7, SM_X2NEG = 8, SM_DZ = 9,
    SM_DW = 10, SM_ABS = 11, SM_ABSNEG = 12, SM_NOT = 13,
};

enum { RO_POSITION = 0, RO_FOG = 1, RO_POINTSIZE = 2 };

enum { MISC_POSITION = 0, MISC_FACE = 1 };

enum DeclUsage : uint8_t {
    DU_POSITION = 0, DU_BLENDWEIGHT = 1, DU_BLENDINDICES = 2, DU_NORMAL = 3,
    DU_PSIZE = 4, DU_TEXCOORD = 5, DU_TANGENT = 6, DU_BINORMAL = 7,
    DU_TESSFACTOR = 8, DU_POSITIONT = 9, DU_COLOR = 10, DU_FOG = 11,
    DU_DEPTH = 12, DU_SAMPLE = 13,
};

enum SamplerDeclType { STT_UNKNOWN = 0, STT_2D = 2, STT_CUBE = 3, STT_VOLUME = 4 };

static const char* kCmpStr[8] = { "??", ">", "==", ">=", "<", "!=", "<=", "??" };

// One decoded source or destination operand.
//
// A D3D9 operand is a register file plus an index, decorated with a write mask
// (destination) or a swizzle (source), an optional modifier such as negate or
// absolute value, and an optional result shift. The defaults below are the
// encodings that mean "no decoration": writeMask 0xF is .xyzw, and swizzle
// 0xE4 is the identity .xyzw — it packs components 3,2,1,0 two bits each, so
// 0b11'10'01'00.
//
// `relative` marks indirect addressing, c[a0.x + 4] and the like, which the
// emitter has to write as a genuine dynamic array index.
struct Operand {
    uint8_t  file      = RF_TEMP;
    uint16_t index     = 0;
    uint8_t  writeMask = 0xF;
    uint8_t  swizzle   = 0xE4;
    uint8_t  mod       = SM_NONE;
    int8_t   shift     = 0;
    bool     relative  = false;
    uint8_t  relFile   = RF_ADDR;
    uint8_t  relComp   = 0;
};

struct Inst {
    uint16_t op = OP_NOP;
    uint8_t  ctrl = 0;
    bool     predicated = false;
    bool     hasDst = false;
    Operand  dst;
    Operand  pred;
    Operand  src[4];
    int      nsrc = 0;

    uint32_t imm[4] = {};

    uint8_t  dclUsage = 0, dclUsageIndex = 0, dclTexType = 0;
};

// Shader model, decoded from the version token. The predicates exist because
// the four model generations differ in ways that matter at almost every step:
// ps_1_3 and earlier address texture stages positionally and write their
// result to r0; ps_1_4 splits sampling from arithmetic; sm3 replaces the fixed
// semantic registers with declared inputs and outputs. Naming the distinction
// once here keeps those checks readable further down.
struct Version {
    bool     ps = false;
    uint32_t major = 0, minor = 0;
    bool  sm1() const { return major == 1; }
    bool  sm3() const { return major >= 3; }
    bool  ps14() const { return ps && major == 1 && minor >= 4; }
    bool  ps13OrOlder() const { return ps && major == 1 && minor <= 3; }
};

struct OpCount { int8_t hasDst; int8_t nsrc; };

static OpCount OperandCounts(uint16_t op, const Version& v)
{
    switch (op) {
    case OP_NOP: case OP_PHASE:                       return { 0, 0 };
    case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG:
    case OP_LIT: case OP_FRC: case OP_EXPP: case OP_LOGP: case OP_ABS:
    case OP_NRM: case OP_MOVA: case OP_DSX: case OP_DSY:
                                                      return { 1, 1 };
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4:
    case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE: case OP_DST:
    case OP_POW: case OP_CRS:
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2:
                                                      return { 1, 2 };
    case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: case OP_DP2ADD:
    case OP_SGN:                                      return { 1, 3 };
    case OP_SINCOS:      return v.major >= 3 ? OpCount{ 1, 1 } : OpCount{ 1, 3 };
    case OP_TEXKILL:                                  return { 1, 0 };
    case OP_TEXCOORD:    return v.ps14() ? OpCount{ 1, 1 } : OpCount{ 1, 0 };
    case OP_TEX:
        if (!v.sm1())   return { 1, 2 };
        return v.ps14() ? OpCount{ 1, 1 } : OpCount{ 1, 0 };
    case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB:
    case OP_TEXREG2RGB: case OP_TEXM3x2PAD: case OP_TEXM3x2TEX:
    case OP_TEXM3x3PAD: case OP_TEXM3x3TEX: case OP_TEXM3x3VSPEC:
    case OP_TEXM3x3: case OP_TEXDP3: case OP_TEXDP3TEX: case OP_TEXM3x2DEPTH:
    case OP_TEXM3x3DIFF:                              return { 1, 1 };
    case OP_TEXM3x3SPEC:                              return { 1, 2 };
    case OP_TEXDEPTH:                                 return { 1, 0 };
    case OP_TEXLDL:                                   return { 1, 2 };
    case OP_TEXLDD:                                   return { 1, 4 };
    case OP_BEM:                                      return { 1, 2 };
    case OP_SETP:                                     return { 1, 2 };
    case OP_IF: case OP_REP: case OP_BREAKP: case OP_CALL: case OP_LABEL:
                                                      return { 0, 1 };
    case OP_IFC: case OP_BREAKC: case OP_LOOP: case OP_CALLNZ:
                                                      return { 0, 2 };
    case OP_ELSE: case OP_ENDIF: case OP_ENDREP: case OP_ENDLOOP:
    case OP_BREAK: case OP_RET:                       return { 0, 0 };
    case OP_DCL: case OP_DEF: case OP_DEFI: case OP_DEFB:
                                                      return { 0, 0 };
    default:                                          return { 0, -1 };
    }
}

// Decodes a token stream into a flat instruction list.
//
// The bytecode is a sequence of 32-bit tokens. Token 0 is the version; after
// that each instruction begins with an opcode token carrying its own length,
// which is what makes the format walkable without knowing every opcode. The
// stream ends at 0x0000FFFF.
//
// The parser is deliberately strict — an unrecognised opcode or a malformed
// operand aborts the whole translation with a message rather than being
// skipped. A shader that translates with one instruction quietly missing
// produces subtly wrong pixels, which is far harder to track down than a
// shader that refuses to compile and says why.
class Parser {
public:
    Parser(const uint32_t* toks, size_t maxDwords) : m_t(toks), m_n(maxDwords) {}

    bool Parse(Version& ver, std::vector<Inst>& out, std::string& err)
    {
        // 0xFFFF in the high half marks a pixel shader, 0xFFFE a vertex
        // shader; the low half is major.minor.
        if (m_n < 2) { err = "blob too small"; return false; }
        const uint32_t vt = m_t[0];
        const uint32_t hi = vt >> 16;
        if (hi != 0xFFFE && hi != 0xFFFF) { err = "bad version token"; return false; }
        ver.ps    = (hi == 0xFFFF);
        ver.major = (vt >> 8) & 0xFF;
        ver.minor = vt & 0xFF;
        if (ver.major < 1 || ver.major > 3) { err = "unsupported shader model"; return false; }
        m_ver = ver;
        m_pos = 1;

        while (true) {
            if (m_pos >= m_n) { err = "unterminated token stream"; return false; }
            const uint32_t tok = m_t[m_pos];

            if (tok == 0x0000FFFFu) { m_pos++; return true; }

            if ((tok & 0xFFFFu) == OP_COMMENT && !(tok & 0x80000000u)) {
                const uint32_t len = (tok >> 16) & 0x7FFF;
                m_pos += 1 + len;
                continue;
            }

            Inst ins{};
            ins.op         = static_cast<uint16_t>(tok & 0xFFFF);
            ins.ctrl       = static_cast<uint8_t>((tok >> 16) & 0xFF);
            ins.predicated = (tok & 0x10000000u) != 0;
            const uint32_t lenField = (tok >> 24) & 0xF;
            const size_t   instStart = m_pos;
            m_pos++;

            if (ins.op == OP_DEF)  { if (!ParseDef(ins, 4, err))  return false; }
            else if (ins.op == OP_DEFI) { if (!ParseDef(ins, 4, err)) return false; }
            else if (ins.op == OP_DEFB) { if (!ParseDef(ins, 1, err)) return false; }
            else if (ins.op == OP_DCL)  { if (!ParseDcl(ins, err))    return false; }
            else {
                const OpCount oc = OperandCounts(ins.op, m_ver);
                if (oc.nsrc < 0) {

                    if (!m_ver.sm1() && lenField > 0) {
                        char b[64];
                        std::snprintf(b, sizeof(b), "unknown opcode %u (skipped)", ins.op);
                        err = b;
                        return false;
                    }
                    char b[48];
                    std::snprintf(b, sizeof(b), "unknown SM1 opcode %u", ins.op);
                    err = b; return false;
                }
                ins.hasDst = oc.hasDst != 0;
                if (ins.hasDst && !ParseDst(ins.dst, err)) return false;
                if (ins.predicated && !ParseSrc(ins.pred, err)) return false;
                ins.nsrc = oc.nsrc;
                for (int i = 0; i < oc.nsrc; ++i)
                    if (!ParseSrc(ins.src[i], err)) return false;
            }

            if (!m_ver.sm1()) {
                const size_t byLen = instStart + 1 + lenField;
                if (byLen > m_pos) m_pos = byLen;
            }

            out.push_back(ins);
        }
    }

private:
    bool Fetch(uint32_t& tok, std::string& err)
    {
        if (m_pos >= m_n) { err = "token stream overrun"; return false; }
        tok = m_t[m_pos++];
        return true;
    }

    static uint8_t FileOf(uint32_t tok)
    {
        return static_cast<uint8_t>(((tok >> 28) & 0x7) | ((tok >> 8) & 0x18));
    }

    bool ParseDst(Operand& o, std::string& err)
    {
        uint32_t tok; if (!Fetch(tok, err)) return false;
        o.file      = FileOf(tok);
        o.index     = static_cast<uint16_t>(tok & 0x7FF);
        o.writeMask = static_cast<uint8_t>((tok >> 16) & 0xF);
        if (o.writeMask == 0) o.writeMask = 0xF;
        o.mod       = static_cast<uint8_t>((tok >> 20) & 0xF);
        int8_t sh   = static_cast<int8_t>((tok >> 24) & 0xF);
        o.shift     = (sh > 7) ? static_cast<int8_t>(sh - 16) : sh;
        o.relative  = (tok & 0x2000u) != 0;
        if (o.relative && !m_ver.sm1()) {
            uint32_t rel; if (!Fetch(rel, err)) return false;
            o.relFile = FileOf(rel);
            o.relComp = static_cast<uint8_t>((rel >> 16) & 0x3);
        }
        return true;
    }

    bool ParseSrc(Operand& o, std::string& err)
    {
        uint32_t tok; if (!Fetch(tok, err)) return false;
        o.file     = FileOf(tok);
        o.index    = static_cast<uint16_t>(tok & 0x7FF);
        o.swizzle  = static_cast<uint8_t>((tok >> 16) & 0xFF);
        o.mod      = static_cast<uint8_t>((tok >> 24) & 0xF);
        o.relative = (tok & 0x2000u) != 0;
        if (o.relative && !m_ver.sm1()) {
            uint32_t rel; if (!Fetch(rel, err)) return false;
            o.relFile = FileOf(rel);
            o.relComp = static_cast<uint8_t>((rel >> 16) & 0x3);
        }
        return true;
    }

    bool ParseDef(Inst& ins, int nImm, std::string& err)
    {
        ins.hasDst = true;
        if (!ParseDst(ins.dst, err)) return false;
        for (int i = 0; i < nImm; ++i)
            if (!Fetch(ins.imm[i], err)) return false;
        return true;
    }

    bool ParseDcl(Inst& ins, std::string& err)
    {
        uint32_t usageTok; if (!Fetch(usageTok, err)) return false;
        ins.hasDst = true;
        if (!ParseDst(ins.dst, err)) return false;
        ins.dclUsage      = static_cast<uint8_t>(usageTok & 0x1F);
        ins.dclUsageIndex = static_cast<uint8_t>((usageTok >> 16) & 0xF);
        ins.dclTexType    = static_cast<uint8_t>((usageTok >> 27) & 0xF);
        return true;
    }

    const uint32_t* m_t;
    size_t          m_n;
    size_t          m_pos = 0;
    Version         m_ver{};
};

static const char kComp[4] = { 'x', 'y', 'z', 'w' };

static const char* SemanticFromUsage(uint8_t usage)
{
    switch (usage) {
    case DU_POSITION:     return "POSITION";
    case DU_BLENDWEIGHT:  return "BLENDWEIGHT";
    case DU_BLENDINDICES: return "BLENDINDICES";
    case DU_NORMAL:       return "NORMAL";
    case DU_PSIZE:        return "PSIZE";
    case DU_TEXCOORD:     return "TEXCOORD";
    case DU_TANGENT:      return "TANGENT";
    case DU_BINORMAL:     return "BINORMAL";
    case DU_TESSFACTOR:   return "TESSFACTOR";
    case DU_POSITIONT:    return "POSITIONT";
    case DU_COLOR:        return "COLOR";
    case DU_FOG:          return "FOG";
    case DU_DEPTH:        return "DEPTH";
    case DU_SAMPLE:       return "SAMPLE";
    default:              return "TEXCOORD";
    }
}

static void AppendFloat(std::string& s, float f)
{
    if (std::isnan(f) || std::isinf(f)) {
        uint32_t bits; std::memcpy(&bits, &f, 4);
        char b[32]; std::snprintf(b, sizeof(b), "asfloat(0x%08Xu)", bits);
        s += b;
        return;
    }
    char b[48];
    std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(f));
    s += b;
    if (!std::strchr(b, '.') && !std::strchr(b, 'e') && !std::strchr(b, 'E'))
        s += ".0";
}

class Emitter {
public:
    Emitter(const Version& v, const std::vector<Inst>& ir,
            const Sm3TranslateOptions& opt, Sm3TranslateResult& out)
        : m_v(v), m_ir(ir), m_opt(opt), m_out(out) {}

    bool Run()
    {
        if (!Scan()) return false;
        EmitAll();
        return m_err.empty();
    }

    std::string TakeError() { return m_err; }

private:

    struct VsInput  { uint16_t reg; uint8_t usage, index; bool asUint; };
    struct SemDecl  { uint16_t reg; uint8_t usage, index; uint8_t mask; bool centroid = false; };

    const Version&             m_v;
    const std::vector<Inst>&   m_ir;
    const Sm3TranslateOptions& m_opt;
    Sm3TranslateResult&        m_out;
    std::string                m_err;

    uint32_t m_tempMask = 0;
    uint32_t m_psTLocalMask = 0;
    uint32_t m_psTcInMask = 0;
    uint32_t m_psColInMask = 0;
    uint32_t m_oTMask = 0;
    bool     m_oD[2] = {};
    bool     m_writesOPos = false;
    uint32_t m_oCMask = 0;
    bool     m_usesA0 = false;
    bool     m_usesPred = false;
    bool     m_usesLoopReg = false;
    int      m_maxLoopDepth = 0;
    bool     m_defc[256] = {};
    bool     m_defi[16]  = {};
    bool     m_defb[16]  = {};
    std::vector<VsInput> m_vsInputs;
    std::vector<SemDecl> m_sm3Out;
    std::vector<SemDecl> m_sm3PsIn;
    int      m_sm3PosOutReg = -1;
    uint32_t m_texm32PadMask = 0;
    uint32_t m_texm33PadMask = 0;
    bool     m_usesODepthLocal = false;

    std::string m_s;
    int         m_indent = 1;
    std::vector<int> m_loopStack;
    int         m_loopCounter = 0;

    bool Scan()
    {
        int loopDepth = 0;
        for (const Inst& I : m_ir) {
            switch (I.op) {
            case OP_DCL:
                if (!ScanDcl(I)) return false;
                continue;
            case OP_DEF:  if (I.dst.file == RF_CONST && I.dst.index < 256) m_defc[I.dst.index] = true; continue;
            case OP_DEFI: if (I.dst.file == RF_CONSTINT && I.dst.index < 16) m_defi[I.dst.index] = true; continue;
            case OP_DEFB: if (I.dst.file == RF_CONSTBOOL && I.dst.index < 16) m_defb[I.dst.index] = true; continue;
            case OP_LOOP: case OP_REP:
                loopDepth++;
                if (loopDepth > m_maxLoopDepth) m_maxLoopDepth = loopDepth;
                break;
            case OP_ENDLOOP: case OP_ENDREP:
                loopDepth--;
                break;
            case OP_CALL: case OP_CALLNZ: case OP_RET: case OP_LABEL:
                m_err = "call/label flow control is not supported (not emitted by fxc for vs/ps 1-3)";
                return false;
            default: break;
            }

            if (I.hasDst) ScanOperand(I.dst, true, I);
            if (I.predicated) m_usesPred = true;
            for (int i = 0; i < I.nsrc; ++i) ScanOperand(I.src[i], false, I);

            if (m_v.ps && m_v.sm1()) ScanPs1TexOp(I);

            if (I.op == OP_TEXKILL && m_v.ps && !m_v.sm1()) {   }
            if (I.op == OP_TEXBEM || I.op == OP_TEXBEML || I.op == OP_BEM)
                m_out.usesBumpEnv = true;
        }

        if (m_v.ps && m_v.sm1()) {
            m_oCMask |= 1;
            m_tempMask |= 1;
        }

        if (!m_v.ps && !m_writesOPos && m_sm3PosOutReg < 0) {
            m_err = "vertex shader never writes oPos";
            return false;
        }
        return true;
    }

    bool ScanDcl(const Inst& I)
    {
        const Operand& d = I.dst;
        if (!m_v.ps) {
            if (d.file == RF_INPUT) {
                VsInput in{};
                in.reg    = d.index;
                in.usage  = I.dclUsage;
                in.index  = I.dclUsageIndex;
                in.asUint = (I.dclUsage == DU_BLENDINDICES);
                m_vsInputs.push_back(in);
            } else if (d.file == RF_TEXCRDOUT && m_v.sm3()) {
                SemDecl o{};
                o.reg = d.index; o.usage = I.dclUsage; o.index = I.dclUsageIndex;
                o.mask = d.writeMask;
                if (o.usage == DU_POSITION && o.index == 0) m_sm3PosOutReg = o.reg;
                m_sm3Out.push_back(o);
            } else if (d.file == RF_SAMPLER) {
                MarkSampler(d.index, I.dclTexType);
            }
        } else {
            if (d.file == RF_SAMPLER) {
                MarkSampler(d.index, I.dclTexType);
            } else if (m_v.sm3()) {
                if (d.file == RF_INPUT) {
                    SemDecl in{};
                    in.reg = d.index; in.usage = I.dclUsage; in.index = I.dclUsageIndex;
                    in.mask = d.writeMask;
                    in.centroid = (d.mod & 0x4u) != 0;
                    m_sm3PsIn.push_back(in);
                } else if (d.file == RF_MISC) {
                    if (d.index == MISC_POSITION) m_out.usesVPos = true;
                    else if (d.index == MISC_FACE) m_out.usesVFace = true;
                }
            } else {

                if (d.file == RF_ADDR)  m_psTcInMask  |= (1u << d.index);
                if (d.file == RF_INPUT) m_psColInMask |= (1u << d.index);
            }
        }
        return true;
    }

    void MarkSampler(int slot, uint8_t dclType)
    {
        if (slot < 0 || slot > 15) return;
        m_out.samplerMask |= (1u << slot);
        switch (dclType) {
        case STT_CUBE:   m_out.samplerTypes[slot] = 1; break;
        case STT_VOLUME: m_out.samplerTypes[slot] = 2; break;
        default:              break;
        }
    }

    void ScanOperand(const Operand& o, bool isDst, const Inst& I)
    {
        switch (o.file) {
        case RF_TEMP:
            if (o.index < 32) m_tempMask |= (1u << o.index);
            break;
        case RF_ADDR:
            if (!m_v.ps) m_usesA0 = true;
            else {
                if (m_v.sm1()) m_psTLocalMask |= (1u << o.index);
                else           m_psTcInMask   |= (1u << o.index);
                if (m_v.sm1()) m_psTcInMask   |= (1u << o.index);
            }
            break;
        case RF_INPUT:
            if (m_v.ps && !m_v.sm3()) m_psColInMask |= (1u << o.index);
            break;
        case RF_RASTOUT:
            if (isDst) {
                if (o.index == RO_POSITION) m_writesOPos = true;
                else if (o.index == RO_FOG)  m_out.writesFog = true;
                else if (o.index == RO_POINTSIZE) m_out.writesPSize = true;
            }
            break;
        case RF_ATTROUT:
            if (isDst && o.index < 2) m_oD[o.index] = true;
            break;
        case RF_TEXCRDOUT:
            if (isDst && !m_v.ps && !m_v.sm3()) m_oTMask |= (1u << o.index);
            break;
        case RF_COLOROUT:
            if (isDst) {
                m_oCMask |= (1u << o.index);
                if (o.index >= 1) m_out.writesOC1Plus = true;
            }
            break;
        case RF_DEPTHOUT:
            if (isDst) { m_out.writesDepth = true; m_usesODepthLocal = true; }
            break;
        case RF_SAMPLER:
            MarkSampler(o.index, STT_UNKNOWN);
            if (!m_v.ps) m_out.usesVSTexture = true;
            break;
        case RF_PREDICATE:
            m_usesPred = true;
            break;
        case RF_LOOP:
            m_usesLoopReg = true;
            break;
        case RF_MISC:
            if (o.index == MISC_POSITION) m_out.usesVPos = true;
            else if (o.index == MISC_FACE) m_out.usesVFace = true;
            break;
        default: break;
        }
        if (o.relative && !m_v.ps) {
            if (o.relFile == RF_LOOP) m_usesLoopReg = true; else m_usesA0 = true;
        }
        (void)I;
    }

    void ScanPs1TexOp(const Inst& I)
    {
        const int m = I.dst.index;
        auto useStage = [&](int s, uint8_t type) {
            if (s < 0 || s > 15) return;
            m_out.samplerMask |= (1u << s);
            if (type != STT_UNKNOWN) {
                m_out.samplerTypes[s] = (type == STT_CUBE) ? 1 :
                                        (type == STT_VOLUME) ? 2 : 0;
            }
            m_psTcInMask |= (1u << s);
        };
        switch (I.op) {
        case OP_TEX:
            if (m_v.ps14()) useStage(m, STT_UNKNOWN);
            else            useStage(m, STT_UNKNOWN);
            break;
        case OP_TEXCOORD:
            m_psTcInMask |= (1u << m);
            break;
        case OP_TEXBEM: case OP_TEXBEML:
        case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXREG2RGB:
        case OP_TEXDP3TEX:
            useStage(m, STT_UNKNOWN);
            break;
        case OP_TEXM3x2PAD:
            m_psTcInMask |= (1u << m); m_texm32PadMask |= (1u << m);
            break;
        case OP_TEXM3x2TEX:
            useStage(m, STT_UNKNOWN);
            break;
        case OP_TEXM3x3PAD:
            m_psTcInMask |= (1u << m); m_texm33PadMask |= (1u << m);
            break;
        case OP_TEXM3x3TEX: case OP_TEXM3x3VSPEC: case OP_TEXM3x3SPEC:
            useStage(m, STT_CUBE);
            break;
        case OP_TEXM3x3: case OP_TEXDP3:
            m_psTcInMask |= (1u << m);
            break;
        case OP_TEXM3x2DEPTH:
            m_psTcInMask |= (1u << m);
            m_out.writesDepth = true; m_usesODepthLocal = true;
            break;
        case OP_TEXDEPTH:
            m_out.writesDepth = true; m_usesODepthLocal = true;
            break;
        default: break;
        }

        if (m_v.ps13OrOlder()) {
            switch (I.op) {
            case OP_TEX: case OP_TEXCOORD: case OP_TEXBEM: case OP_TEXBEML:
            case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXREG2RGB:
            case OP_TEXM3x2TEX: case OP_TEXM3x3TEX: case OP_TEXM3x3SPEC:
            case OP_TEXM3x3VSPEC: case OP_TEXM3x3: case OP_TEXDP3:
            case OP_TEXDP3TEX:
                m_psTLocalMask |= (1u << I.dst.index);
                break;
            default: break;
            }
        }
    }

    std::string Idx(int n) { return std::to_string(n); }

    std::string ConstRef(const Operand& o)
    {
        std::string base;
        if (!o.relative && o.file == RF_CONST && o.index < 256 && m_defc[o.index])
            return "defc_" + Idx(o.index);
        base = "c[";
        if (o.relative) {
            if (o.relFile == RF_LOOP) base += CurrentLoopVar();
            else { base += "a0."; base += kComp[o.relComp & 3]; }
            base += " + ";
        }
        base += Idx(o.index);
        base += "]";
        return base;
    }

    std::string CurrentLoopVar()
    {

        for (auto it = m_loopStack.rbegin(); it != m_loopStack.rend(); ++it)
            if (*it >= 0)
                return "aL" + Idx(*it);
        return "0";
    }

    std::string RegName(const Operand& o, bool asDst)
    {
        switch (o.file) {
        case RF_TEMP:      return "r" + Idx(o.index);
        case RF_INPUT:
            if (!m_v.ps) {
                for (const VsInput& in : m_vsInputs)
                    if (in.reg == o.index && in.asUint) return "_vf" + Idx(o.index);
                return "In.v" + Idx(o.index);
            }
            if (m_v.sm3()) return "In.v" + Idx(o.index);
            return "In.col" + Idx(o.index);
        case RF_CONST:     return ConstRef(o);
        case RF_ADDR:
            if (!m_v.ps) return "a0";

            if (m_v.ps13OrOlder()) return "t" + Idx(o.index);
            if (m_v.ps14())        return asDst ? ("r" + Idx(o.index)) : ("In.tc[" + Idx(o.index) + "]");
            return "In.tc[" + Idx(o.index) + "]";
        case RF_RASTOUT:
            if (o.index == RO_POSITION) return "O.oPos";
            if (o.index == RO_FOG)      return "O.oFog";
            return "O.oPts";
        case RF_ATTROUT:   return "O.oD" + Idx(o.index);
        case RF_TEXCRDOUT:
            if (m_v.sm3()) return "O.o" + Idx(o.index);
            return "O.oT[" + Idx(o.index) + "]";
        case RF_CONSTINT:
            if (o.index < 16 && m_defi[o.index]) return "defi_" + Idx(o.index);
            return "ic[" + Idx(o.index) + "]";
        case RF_COLOROUT:  return "oC" + Idx(o.index);
        case RF_DEPTHOUT:  return "oDepth";
        case RF_CONSTBOOL: {
            if (o.index < 16 && m_defb[o.index]) return "defb_" + Idx(o.index);
            std::string s = "(bc[" + Idx(o.index >> 2) + "].";
            s += kComp[o.index & 3];
            s += " != 0u)";
            return s;
        }
        case RF_LOOP:      return CurrentLoopVar();
        case RF_MISC:
            return (o.index == MISC_POSITION) ? "vPos" : "vFace";
        case RF_PREDICATE: return "p0";
        case RF_SAMPLER:

            return "smp" + Idx(o.index);
        default:
            m_err = "unsupported register file " + Idx(o.file);
            return "r0";
        }
    }

    static bool IsIdentitySwizzle(uint8_t s) { return s == 0xE4; }

    static std::string SwizzleStr(uint8_t swz, int keep = 4)
    {
        std::string s = ".";
        for (int i = 0; i < keep; ++i)
            s += kComp[(swz >> (i * 2)) & 3];
        return s;
    }

    std::string Src(const Operand& o)
    {
        std::string e = RegName(o, false);

        if (o.file == RF_CONSTBOOL) {
            if (o.mod == SM_NOT) return "(!" + e + ")";
            return e;
        }
        if (o.file == RF_LOOP) return e;

        std::string base = e;
        if (!IsIdentitySwizzle(o.swizzle)) e += SwizzleStr(o.swizzle);

        switch (o.mod) {
        case SM_NONE:    break;
        case SM_NEG:     e = "(-" + e + ")";                          break;
        case SM_BIAS:    e = "(" + e + " - 0.5)";                     break;
        case SM_BIASNEG: e = "(0.5 - " + e + ")";                     break;
        case SM_SIGN:    e = "(" + e + " * 2.0 - 1.0)";               break;
        case SM_SIGNNEG: e = "(1.0 - " + e + " * 2.0)";               break;
        case SM_COMP:    e = "(1.0 - " + e + ")";                     break;
        case SM_X2:      e = "(" + e + " * 2.0)";                     break;
        case SM_X2NEG:   e = "(" + e + " * -2.0)";                    break;
        case SM_DZ:      e = "(" + e + " / " + base + ".z)";          break;
        case SM_DW:      e = "(" + e + " / " + base + ".w)";          break;
        case SM_ABS:     e = "abs(" + e + ")";                        break;
        case SM_ABSNEG:  e = "(-abs(" + e + "))";                     break;
        case SM_NOT:     e = "(!" + e + ")";                          break;
        default: break;
        }
        if (e[0] != '(' && o.mod == SM_NONE && IsIdentitySwizzle(o.swizzle))
            return e;
        return "(" + e + ")";
    }

    static std::string MaskStr(uint8_t m)
    {
        if (m == 0xF) return "";
        std::string s = ".";
        for (int i = 0; i < 4; ++i)
            if (m & (1 << i)) s += kComp[i];
        return s;
    }

    void Line(const std::string& t)
    {
        for (int i = 0; i < m_indent; ++i) m_s += "    ";
        m_s += t;
        m_s += "\n";
    }

    void Store(const Inst& I, std::string rhs, bool rhsScalar = false)
    {
        const Operand& d = I.dst;

        if (d.relative && m_err.empty()) {
            m_err = "relative-addressed destination register (o#[aL]) is not supported";
            return;
        }

        if (d.shift != 0) {
            const double f = std::ldexp(1.0, d.shift);
            char b[32]; std::snprintf(b, sizeof(b), "%g", f);
            rhs = "((" + rhs + ") * " + b + ")";
        }
        if (d.mod & 0x1) rhs = "saturate(" + rhs + ")";

        if (d.file == RF_ATTROUT && !m_v.ps && !m_v.sm3() && !(d.mod & 0x1))
            rhs = "saturate(" + rhs + ")";

        const std::string mask = MaskStr(d.writeMask);

        if (d.file == RF_ADDR && !m_v.ps) {

            const char* cvt = (I.op == OP_MOVA) ? "round" : "floor";
            std::string val = "int4(" + std::string(cvt) + "(" + rhs + "))";
            EmitPredicated(I, "a0" + mask, val + (rhsScalar ? "" : mask), "a0");
            return;
        }

        if (d.file == RF_RASTOUT && d.index != RO_POSITION) {
            EmitPredicated(I, RegName(d, true), "(" + rhs + ")" + (rhsScalar ? "" : ".x"),
                           RegName(d, true));
            return;
        }
        if (d.file == RF_DEPTHOUT) {
            EmitPredicated(I, "oDepth", "(" + rhs + ")" + (rhsScalar ? "" : ".x"), "oDepth");
            return;
        }

        const std::string dst = RegName(d, true);
        std::string sel = rhsScalar ? "" : mask;
        std::string value = rhsScalar ? rhs : ("(" + rhs + ")" + sel);
        EmitPredicated(I, dst + mask, value, dst);
    }

    void EmitPredicated(const Inst& I, const std::string& lhs,
                        const std::string& value, const std::string& dstBase)
    {
        if (!I.predicated) {
            Line(lhs + " = " + value + ";");
            return;
        }

        std::string p = "p0";
        if (!IsIdentitySwizzle(I.pred.swizzle)) p += SwizzleStr(I.pred.swizzle);
        if (I.pred.mod == SM_NOT) p = "(!" + p + ")";
        const std::string mask = MaskStr(I.dst.writeMask);
        std::string maskedPred = p;
        if (!mask.empty()) maskedPred = "(" + p + ")" + mask;
        Line(lhs + " = (" + maskedPred + ") ? " + value + " : " + dstBase + mask + ";");
    }

    std::string SrcRow(const Operand& o, int row)
    {
        Operand r = o;
        r.index = static_cast<uint16_t>(r.index + row);
        r.swizzle = 0xE4;
        r.mod = SM_NONE;
        return Src(r);
    }

    std::string CoordSel(int slot, const std::string& c) const
    {
        return "DX9_CRD" + std::to_string(slot & 15) + "(" + c + ")";
    }

    std::string Sample(int slot, const std::string& coordF4) const
    {
        return "tex" + std::to_string(slot) + ".Sample(smp" + std::to_string(slot)
             + ", " + CoordSel(slot, coordF4) + ")";
    }

    void EmitInst(const Inst& I)
    {
        std::string a = (I.nsrc > 0) ? Src(I.src[0]) : std::string();
        std::string b = (I.nsrc > 1) ? Src(I.src[1]) : std::string();
        std::string c = (I.nsrc > 2) ? Src(I.src[2]) : std::string();

        switch (I.op) {
        case OP_NOP: case OP_PHASE: return;
        case OP_DCL: case OP_DEF: case OP_DEFI: case OP_DEFB: return;

        case OP_MOV:  Store(I, a); return;
        case OP_ADD:  Store(I, a + " + " + b); return;
        case OP_SUB:  Store(I, a + " - " + b); return;
        case OP_MUL:  Store(I, a + " * " + b); return;
        case OP_MAD:  Store(I, a + " * " + b + " + " + c); return;
        case OP_LRP:  Store(I, "lerp(" + c + ", " + b + ", " + a + ")"); return;
        case OP_FRC:  Store(I, "frac(" + a + ")"); return;
        case OP_MIN:  Store(I, "min(" + a + ", " + b + ")"); return;
        case OP_MAX:  Store(I, "max(" + a + ", " + b + ")"); return;
        case OP_ABS:  Store(I, "abs(" + a + ")"); return;
        case OP_SGN:  Store(I, "sign(" + a + ")"); return;
        case OP_SLT:  Store(I, "float4(" + a + " < "  + b + ")"); return;
        case OP_SGE:  Store(I, "float4(" + a + " >= " + b + ")"); return;
        case OP_CND:  Store(I, "((" + a + " > 0.5) ? " + b + " : " + c + ")"); return;
        case OP_CMP:  Store(I, "((" + a + " >= 0.0) ? " + b + " : " + c + ")"); return;
        case OP_DSX:  Store(I, "ddx(" + a + ")"); return;
        case OP_DSY:  Store(I, "ddy(" + a + ")"); return;
        case OP_MOVA: Store(I, a); return;

        case OP_RCP:  Store(I, "(1.0 / " + a + ".x)", true); return;
        case OP_RSQ:  Store(I, "rsqrt(abs(" + a + ".x))", true); return;
        case OP_EXP: case OP_EXPP: Store(I, "exp2(" + a + ".x)", true); return;
        case OP_LOG: case OP_LOGP: Store(I, "log2(abs(" + a + ".x))", true); return;
        case OP_POW:  Store(I, "pow(abs(" + a + ".x), " + b + ".x)", true); return;
        case OP_DP3:  Store(I, "dot(" + a + ".xyz, " + b + ".xyz)", true); return;
        case OP_DP4:  Store(I, "dot(" + a + ", " + b + ")", true); return;
        case OP_DP2ADD:
            Store(I, "(dot(" + a + ".xy, " + b + ".xy) + " + c + ".x)", true); return;

        case OP_NRM:
            Store(I, a + " * rsqrt(max(dot(" + a + ".xyz, " + a + ".xyz), 1e-10))");
            return;
        case OP_CRS:
            Store(I, "float4(cross(" + a + ".xyz, " + b + ".xyz), 0.0)"); return;
        case OP_LIT:  Store(I, "d3d_lit(" + a + ")"); return;
        case OP_DST:
            Store(I, "float4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)");
            return;
        case OP_SINCOS:

            Store(I, "float4(cos(" + a + ".x), sin(" + a + ".x), 0.0, 0.0)");
            return;

        case OP_M4x4:
            Store(I, "float4(dot(" + a + ", " + SrcRow(I.src[1], 0) + "), "
                     "dot(" + a + ", " + SrcRow(I.src[1], 1) + "), "
                     "dot(" + a + ", " + SrcRow(I.src[1], 2) + "), "
                     "dot(" + a + ", " + SrcRow(I.src[1], 3) + "))");
            return;
        case OP_M4x3:
            Store(I, "float4(dot(" + a + ", " + SrcRow(I.src[1], 0) + "), "
                     "dot(" + a + ", " + SrcRow(I.src[1], 1) + "), "
                     "dot(" + a + ", " + SrcRow(I.src[1], 2) + "), 0.0)");
            return;
        case OP_M3x4:
            Store(I, "float4(dot(" + a + ".xyz, " + SrcRow(I.src[1], 0) + ".xyz), "
                     "dot(" + a + ".xyz, " + SrcRow(I.src[1], 1) + ".xyz), "
                     "dot(" + a + ".xyz, " + SrcRow(I.src[1], 2) + ".xyz), "
                     "dot(" + a + ".xyz, " + SrcRow(I.src[1], 3) + ".xyz))");
            return;
        case OP_M3x3:
            Store(I, "float4(dot(" + a + ".xyz, " + SrcRow(I.src[1], 0) + ".xyz), "
                     "dot(" + a + ".xyz, " + SrcRow(I.src[1], 1) + ".xyz), "
                     "dot(" + a + ".xyz, " + SrcRow(I.src[1], 2) + ".xyz), 0.0)");
            return;
        case OP_M3x2:
            Store(I, "float4(dot(" + a + ".xyz, " + SrcRow(I.src[1], 0) + ".xyz), "
                     "dot(" + a + ".xyz, " + SrcRow(I.src[1], 1) + ".xyz), 0.0, 0.0)");
            return;

        case OP_IF: {

            std::string cond = a;
            if (I.src[0].file == RF_PREDICATE) {
                cond = "p0";
                cond += (IsIdentitySwizzle(I.src[0].swizzle))
                      ? ".x" : SwizzleStr(I.src[0].swizzle, 1);
                if (I.src[0].mod == SM_NOT) cond = "!" + cond;
            }
            Line("if (" + cond + ") {");
            m_indent++; return;
        }
        case OP_IFC:
            Line("if (" + a + ".x " + kCmpStr[I.ctrl & 7] + " " + b + ".x) {");
            m_indent++; return;
        case OP_ELSE:
            m_indent--; Line("} else {"); m_indent++; return;
        case OP_ENDIF:
            m_indent--; Line("}"); return;
        case OP_REP: {
            const int id = m_loopCounter++;
            m_loopStack.push_back(-1);
            Line("[loop] for (int rep" + Idx(id) + " = 0; rep" + Idx(id) + " < "
                 + a + ".x; ++rep" + Idx(id) + ") {");
            m_indent++; return;
        }
        case OP_LOOP: {
            const int id = m_loopCounter++;
            m_loopStack.push_back(id);

            Line("[loop] for (int aL" + Idx(id) + " = " + b + ".y, _it" + Idx(id)
                 + " = 0; _it" + Idx(id) + " < " + b + ".x; ++_it" + Idx(id)
                 + ", aL" + Idx(id) + " += " + b + ".z) {");
            m_indent++; return;
        }
        case OP_ENDREP: case OP_ENDLOOP:
            if (!m_loopStack.empty()) m_loopStack.pop_back();
            m_indent--; Line("}"); return;
        case OP_BREAK:
            Line("break;"); return;
        case OP_BREAKC:
            Line("if (" + a + ".x " + kCmpStr[I.ctrl & 7] + " " + b + ".x) break;");
            return;
        case OP_BREAKP: {
            std::string p = "p0";
            if (!IsIdentitySwizzle(I.src[0].swizzle)) p += SwizzleStr(I.src[0].swizzle, 1);
            else p += ".x";
            if (I.src[0].mod == SM_NOT) p = "!" + p;
            Line("if (" + p + ") break;"); return;
        }
        case OP_SETP: {
            const std::string mask = MaskStr(I.dst.writeMask);
            Line("p0" + mask + " = (" + a + " " + kCmpStr[I.ctrl & 7] + " " + b + ")"
                 + mask + ";");
            return;
        }

        case OP_TEX:
            if (!m_v.sm1()) { EmitTexld(I, a); return; }
            EmitPs1Tex(I, a); return;
        case OP_TEXLDL: {
            const int s = I.src[1].index;
            Store(I, "tex" + Idx(s) + ".SampleLevel(smp" + Idx(s) + ", "
                  + CoordSel(s, a) + ", " + a + ".w)");
            return;
        }
        case OP_TEXLDD: {
            const int s = I.src[1].index;
            Store(I, "tex" + Idx(s) + ".SampleGrad(smp" + Idx(s) + ", "
                  + CoordSel(s, a) + ", " + CoordSel(s, Src(I.src[2])) + ", "
                  + CoordSel(s, Src(I.src[3])) + ")");
            return;
        }
        case OP_TEXKILL: {

            Operand asSrc = I.dst; asSrc.swizzle = 0xE4; asSrc.mod = SM_NONE;
            Line("clip(" + RegName(asSrc, false) + ".xyz);");
            return;
        }

        case OP_TEXCOORD:
            if (m_v.ps14()) Store(I, "float4(" + a + ".xyz, 1.0)");
            else Line("t" + Idx(I.dst.index) + " = float4(saturate(In.tc["
                      + Idx(I.dst.index) + "].xyz), 1.0);");
            return;
        case OP_TEXBEM: case OP_TEXBEML: {
            const int m = I.dst.index;
            std::string coord = "In.tc[" + Idx(m) + "].xy + float2(dot(g_bumpMat["
                + Idx(m) + "].xy, " + a + ".xy), dot(g_bumpMat[" + Idx(m)
                + "].zw, " + a + ".xy))";
            std::string s = "tex" + Idx(m) + ".Sample(smp" + Idx(m) + ", " + coord + ")";
            if (I.op == OP_TEXBEML)
                s = "(" + s + ") * (" + a + ".z * g_bumpScaleOff[" + Idx(m)
                  + "].x + g_bumpScaleOff[" + Idx(m) + "].y)";
            Line("t" + Idx(m) + " = " + s + ";");
            return;
        }
        case OP_BEM: {
            const int m = I.dst.index;
            Store(I, "float4(" + a + ".xy + float2(dot(g_bumpMat[" + Idx(m)
                  + "].xy, " + b + ".xy), dot(g_bumpMat[" + Idx(m) + "].zw, "
                  + b + ".xy)), 0.0, 0.0)");
            return;
        }
        case OP_TEXREG2AR: {
            const int m = I.dst.index;
            Line("t" + Idx(m) + " = tex" + Idx(m) + ".Sample(smp" + Idx(m)
                 + ", float2(" + a + ".w, " + a + ".x));");
            return;
        }
        case OP_TEXREG2GB: {
            const int m = I.dst.index;
            Line("t" + Idx(m) + " = tex" + Idx(m) + ".Sample(smp" + Idx(m)
                 + ", float2(" + a + ".y, " + a + ".z));");
            return;
        }
        case OP_TEXREG2RGB: {
            const int m = I.dst.index;
            Line("t" + Idx(m) + " = " + Sample(m, "(" + a + ")") + ";");
            return;
        }
        case OP_TEXM3x2PAD:
            Line("float _m32_" + Idx(I.dst.index) + " = dot(In.tc["
                 + Idx(I.dst.index) + "].xyz, " + a + ".xyz);");
            return;
        case OP_TEXM3x2TEX: {
            const int m = I.dst.index;
            Line("t" + Idx(m) + " = tex" + Idx(m) + ".Sample(smp" + Idx(m)
                 + ", float2(_m32_" + Idx(m - 1) + ", dot(In.tc[" + Idx(m)
                 + "].xyz, " + a + ".xyz)));");
            return;
        }
        case OP_TEXM3x2DEPTH: {
            const int m = I.dst.index;
            Line("{ float _w = dot(In.tc[" + Idx(m) + "].xyz, " + a + ".xyz);");
            Line("  oDepth = (_w == 0.0) ? 1.0 : saturate(_m32_" + Idx(m - 1)
                 + " / _w); }");
            return;
        }
        case OP_TEXM3x3PAD:
            Line("float _m33_" + Idx(I.dst.index) + " = dot(In.tc["
                 + Idx(I.dst.index) + "].xyz, " + a + ".xyz);");
            return;
        case OP_TEXM3x3TEX: case OP_TEXM3x3: case OP_TEXM3x3SPEC:
        case OP_TEXM3x3VSPEC: {
            const int m = I.dst.index;
            const std::string v = "float3(_m33_" + Idx(m - 2) + ", _m33_"
                + Idx(m - 1) + ", dot(In.tc[" + Idx(m) + "].xyz, " + a + ".xyz))";
            if (I.op == OP_TEXM3x3) {
                Line("t" + Idx(m) + " = float4(" + v + ", 1.0);");
            } else if (I.op == OP_TEXM3x3TEX) {
                Line("t" + Idx(m) + " = tex" + Idx(m) + ".Sample(smp" + Idx(m)
                     + ", " + v + ");");
            } else {
                std::string eye;
                if (I.op == OP_TEXM3x3SPEC) eye = b + ".xyz";
                else eye = "float3(In.tc[" + Idx(m - 2) + "].w, In.tc[" + Idx(m - 1)
                         + "].w, In.tc[" + Idx(m) + "].w)";
                Line("{ float3 _n = " + v + "; float3 _e = " + eye + ";");
                Line("  float3 _r = 2.0 * dot(_n, _e) / dot(_n, _n) * _n - _e;");
                Line("  t" + Idx(m) + " = tex" + Idx(m) + ".Sample(smp" + Idx(m)
                     + ", _r); }");
            }
            return;
        }
        case OP_TEXDP3: {
            const int m = I.dst.index;
            Line("t" + Idx(m) + " = dot(In.tc[" + Idx(m) + "].xyz, " + a
                 + ".xyz).xxxx;");
            return;
        }
        case OP_TEXDP3TEX: {
            const int m = I.dst.index;
            Line("t" + Idx(m) + " = tex" + Idx(m) + ".Sample(smp" + Idx(m)
                 + ", float2(dot(In.tc[" + Idx(m) + "].xyz, " + a + ".xyz), 0.0));");
            return;
        }
        case OP_TEXDEPTH:

            Line("oDepth = (r5.y == 0.0) ? 1.0 : saturate(r5.x / r5.y);");
            return;
        case OP_TEXM3x3DIFF:
            m_err = "texm3x3diff is reserved/unsupported";
            return;

        default: {
            char bb[48];
            std::snprintf(bb, sizeof(bb), "unhandled opcode %u", I.op);
            m_err = bb;
            return;
        }
        }
    }

    void EmitTexld(const Inst& I, const std::string& coord)
    {
        const int s = I.src[1].index;
        const uint8_t variant = I.ctrl & 0x3;
        std::string call;
        if (variant == 1) {
            call = "tex" + Idx(s) + ".Sample(smp" + Idx(s) + ", DX9_PROJ"
                 + Idx(s) + "(" + coord + "))";
        } else if (variant == 2) {
            call = "tex" + Idx(s) + ".SampleBias(smp" + Idx(s) + ", "
                 + CoordSel(s, coord) + ", " + coord + ".w)";
        } else {
            call = Sample(s, coord);
        }
        Store(I, call);
    }

    void EmitPs1Tex(const Inst& I, const std::string& a)
    {
        const int m = I.dst.index;
        if (m_v.ps14()) {

            Store(I, Sample(m, a));
        } else {
            Line("t" + Idx(m) + " = " + Sample(m, "In.tc[" + Idx(m) + "]") + ";");
        }
    }

    void EmitAll()
    {
        m_out.isVertexShader = !m_v.ps;
        m_out.versionMajor   = m_v.major;
        m_out.versionMinor   = m_v.minor;

        std::string s;
        s.reserve(16384);

        char hdr[96];
        std::snprintf(hdr, sizeof(hdr), "// MWOn12 translated %s_%u_%u\n",
                      m_v.ps ? "ps" : "vs", m_v.major, m_v.minor);
        s += hdr;
        s += "#ifndef DX9_ATEST\n#define DX9_ATEST 0\n#endif\n";

        s += "#ifndef DX9_FOG\n#define DX9_FOG 0\n#endif\n\n";

        s += m_v.ps
           ? "cbuffer Dx9PSConst : register(b0) {\n    float4 c[224];\n"
           : "cbuffer Dx9VSConst : register(b0) {\n    float4 c[256];\n";
        s += "    int4   ic[16];\n    uint4  bc[4];\n};\n";

        s += "cbuffer Dx9Emu : register(b1) {\n"
             "    float4 g_emuMisc;          // x=1/vpW y=1/vpH z=alphaRef\n"
             "    float4 g_bumpMat[8];       // M00 M10 M01 M11\n"
             "    float4 g_bumpScaleOff[8];  // x=LSCALE y=LOFFSET\n"
             "    float4 g_fogParams;        // x=FogStart y=FogEnd z=FogDensity\n"
             "    float4 g_fogColor;         // D3DRS_FOGCOLOR, RGBA\n"
             "    float4 g_clipPlanes[6];    // clip-space user planes; zeroed when disabled\n"
             "};\n\n";

        for (int i = 0; i < 16; ++i) {
            if (!(m_out.samplerMask & (1u << i))) continue;
            const std::string kindMacro = "DX9_TEXKIND" + std::to_string(i);
            s += "#ifndef " + kindMacro + "\n#define " + kindMacro + " "
               + std::to_string(m_out.samplerTypes[i]) + "\n#endif\n";
            s += "#if " + kindMacro + " == 1\n"
                 "TextureCube tex" + std::to_string(i) + " : register(t" + std::to_string(i) + ");\n"
                 "#elif " + kindMacro + " == 2\n"
                 "Texture3D   tex" + std::to_string(i) + " : register(t" + std::to_string(i) + ");\n"
                 "#else\n"
                 "Texture2D   tex" + std::to_string(i) + " : register(t" + std::to_string(i) + ");\n"
                 "#endif\n";
            s += "SamplerState smp" + std::to_string(i)
               + " : register(s" + std::to_string(i) + ");\n";

            s += "#if " + kindMacro + " == 0\n"
                 "#define DX9_CRD" + std::to_string(i) + "(c) ((c).xy)\n"
                 "#define DX9_PROJ" + std::to_string(i) + "(c) ((c).xy / (c).w)\n"
                 "#else\n"
                 "#define DX9_CRD" + std::to_string(i) + "(c) ((c).xyz)\n"
                 "#define DX9_PROJ" + std::to_string(i) + "(c) ((c).xyz / (c).w)\n"
                 "#endif\n";
        }
        if (m_out.samplerMask) s += "\n";

        EmitDefs(s);

        if (UsesOp(OP_LIT)) {
            s += "float4 d3d_lit(float4 v) {\n"
                 "    float p = clamp(v.w, -127.9961, 127.9961);\n"
                 "    return float4(1.0, max(v.x, 0.0),\n"
                 "        (v.x > 0.0 && v.y > 0.0) ? pow(v.y, p) : 0.0, 1.0);\n"
                 "}\n\n";
        }

        if (m_v.ps) EmitPS(s); else EmitVS(s);

        m_out.hlsl = std::move(s);
    }

    bool UsesOp(uint16_t op) const
    {
        for (const Inst& I : m_ir) if (I.op == op) return true;
        return false;
    }

    void EmitDefs(std::string& s)
    {
        bool any = false;
        for (const Inst& I : m_ir) {
            if (I.op == OP_DEF && I.dst.file == RF_CONST) {
                float f[4]; std::memcpy(f, I.imm, 16);
                s += "static const float4 defc_" + std::to_string(I.dst.index)
                   + " = float4(";
                for (int i = 0; i < 4; ++i) { if (i) s += ", "; AppendFloat(s, f[i]); }
                s += ");\n"; any = true;
            } else if (I.op == OP_DEFI && I.dst.file == RF_CONSTINT) {
                int32_t v[4]; std::memcpy(v, I.imm, 16);
                s += "static const int4 defi_" + std::to_string(I.dst.index)
                   + " = int4(" + std::to_string(v[0]) + ", " + std::to_string(v[1])
                   + ", " + std::to_string(v[2]) + ", " + std::to_string(v[3]) + ");\n";
                any = true;
            } else if (I.op == OP_DEFB && I.dst.file == RF_CONSTBOOL) {
                s += "static const bool defb_" + std::to_string(I.dst.index)
                   + " = " + (I.imm[0] ? "true" : "false") + ";\n";
                any = true;
            }
        }
        if (any) s += "\n";
    }

    void EmitTempDecls()
    {
        std::string decl;
        for (int i = 0; i < 32; ++i) {
            if (!(m_tempMask & (1u << i))) continue;
            if (!decl.empty()) decl += ", ";
            decl += "r" + std::to_string(i) + " = 0.0";
        }
        if (!decl.empty()) Line("float4 " + decl + ";");

        if (m_v.ps && m_v.ps13OrOlder()) {
            std::string t;
            for (int i = 0; i < 6; ++i) {
                if (!(m_psTLocalMask & (1u << i))) continue;
                if (!t.empty()) t += ", ";
                t += "t" + std::to_string(i) + " = In.tc[" + std::to_string(i) + "]";
            }
            if (!t.empty()) Line("float4 " + t + ";");
        }
        if (m_usesA0 && !m_v.ps)   Line("int4 a0 = 0;");
        if (m_usesPred)            Line("bool4 p0 = false;");
        if (m_usesODepthLocal)     Line("float oDepth = 0.0;");
    }

    void EmitBody()
    {
        for (const Inst& I : m_ir) {
            EmitInst(I);
            if (!m_err.empty()) return;
        }
    }

    void EmitVS(std::string& s)
    {

        const bool hasIn = !m_vsInputs.empty();
        if (hasIn) {
            s += "struct VSIn {\n";
            for (const VsInput& in : m_vsInputs) {
                s += "    ";
                s += in.asUint ? "uint4 " : "float4 ";
                s += "v" + std::to_string(in.reg) + " : ";
                s += SemanticFromUsage(in.usage);
                s += std::to_string(in.index);
                s += ";\n";
            }
            s += "};\n\n";
        }

        s += "struct VSOut {\n";
        if (m_v.sm3()) {

            for (const SemDecl& o : m_sm3Out) {
                if (static_cast<int>(o.reg) == m_sm3PosOutReg) continue;
                s += "    float4 o" + std::to_string(o.reg)
                   + " : TEXCOORD" + std::to_string(o.reg) + ";\n";
            }
            s += "    float4 o" + std::to_string(m_sm3PosOutReg >= 0 ? m_sm3PosOutReg : 0)
               + " : SV_Position;\n";
        } else {

            s += "    float4 oD0  : COLOR0;\n";
            s += "    float4 oD1  : COLOR1;\n";
            s += "    float  oFog : TEXCOORD8;\n";
            s += "    float4 oT[8] : TEXCOORD0;\n";
            if (m_out.writesPSize) s += "    float  oPts : PSIZE0;\n";
            s += "    float4 oPos : SV_Position;\n";
        }

        if (m_opt.emitClipPlanes) {
            s += "    float4 clipD0 : SV_ClipDistance0;\n";
            s += "    float2 clipD1 : SV_ClipDistance1;\n";
        }
        s += "};\n\n";

        s += hasIn ? "VSOut main(VSIn In) {\n" : "VSOut main() {\n";
        m_s = std::move(s);

        Line("VSOut O = (VSOut)0;");

        if (!m_v.sm3() && m_opt.legacyColorDefault) {
            Line("O.oD0 = float4(1.0, 1.0, 1.0, 1.0);   // D3D9 default diffuse");
            Line("O.oD1 = float4(0.0, 0.0, 0.0, 0.0);   // D3D9 default specular");
        }

        for (const VsInput& in : m_vsInputs)
            if (in.asUint)
                Line("float4 _vf" + std::to_string(in.reg) + " = (float4)In.v"
                     + std::to_string(in.reg) + ";");
        EmitTempDecls();
        EmitBody();

        const std::string pos = m_v.sm3()
            ? ("O.o" + std::to_string(m_sm3PosOutReg >= 0 ? m_sm3PosOutReg : 0))
            : "O.oPos";
        if (m_opt.halfPixelFix)
            Line(pos + ".xy += float2(-g_emuMisc.x, g_emuMisc.y) * " + pos + ".w;");

        if (m_opt.emitClipPlanes) {
            Line("O.clipD0 = float4(dot(" + pos + ", g_clipPlanes[0]), "
                 "dot(" + pos + ", g_clipPlanes[1]), "
                 "dot(" + pos + ", g_clipPlanes[2]), "
                 "dot(" + pos + ", g_clipPlanes[3]));");
            Line("O.clipD1 = float2(dot(" + pos + ", g_clipPlanes[4]), "
                 "dot(" + pos + ", g_clipPlanes[5]));");
        }

        Line("return O;");
        m_s += "}\n";
        s = std::move(m_s);
        m_s.clear();
    }

    void EmitPS(std::string& s)
    {

        std::string fields;
        if (m_v.sm3()) {
            if (m_out.usesVPos) fields += "    float4 vpos : SV_Position;\n";

            for (const SemDecl& in : m_sm3PsIn) {

                fields += std::string("    ") + (in.centroid ? "centroid " : "")
                        + "float4 v" + std::to_string(in.reg)
                        + " : TEXCOORD" + std::to_string(in.reg) + ";\n";
            }
            if (m_out.usesVFace) fields += "    bool isFront : SV_IsFrontFace;\n";
        } else {

            fields += "    float4 col0 : COLOR0;\n";
            fields += "    float4 col1 : COLOR1;\n";
            fields += "    float  fog  : TEXCOORD8;\n";
            fields += "    float4 tc[8] : TEXCOORD0;\n";
        }
        const bool hasIn = !fields.empty();
        if (hasIn) s += "struct PSIn {\n" + fields + "};\n\n";

        const bool mrt = m_out.writesOC1Plus || m_out.writesDepth;

        const bool fogNeedsSvPos = !m_out.usesVPos;
        std::string inArg;
        if (hasIn) {
            inArg = "(PSIn In";
            if (fogNeedsSvPos)
                inArg += "\n#if DX9_FOG != 0\n    , float4 _fogSvPos : SV_Position\n#endif\n";
            inArg += ") ";
        } else {
            if (fogNeedsSvPos)
                inArg = "(\n#if DX9_FOG != 0\n    float4 _fogSvPos : SV_Position\n#endif\n) ";
            else
                inArg = "() ";
        }
        if (mrt) {
            s += "struct PSOut {\n";
            for (int i = 0; i < 4; ++i)
                if (m_oCMask & (1u << i))
                    s += "    float4 c" + std::to_string(i)
                       + " : SV_Target" + std::to_string(i) + ";\n";
            if (m_out.writesDepth) s += "    float depth : SV_Depth;\n";
            s += "};\n\n";
            s += std::string("PSOut main") + inArg + "{\n";
        } else {
            s += std::string("float4 main") + inArg + ": SV_Target0 {\n";
        }
        m_s = std::move(s);

        {
            std::string d;
            for (int i = 0; i < 4; ++i) {
                if (!(m_oCMask & (1u << i))) continue;
                if (!d.empty()) d += ", ";
                d += "oC" + std::to_string(i) + " = 0.0";
            }
            if (!d.empty()) Line("float4 " + d + ";");
        }
        // D3D9's vPos is the pixel's top-left corner; SV_Position arrives at
        // the pixel centre, half a texel further on in each axis. Subtracting
        // 0.5 recovers the D3D9 convention, which matters for any shader doing
        // exact texel addressing off vPos.
        if (m_out.usesVPos)
            Line("float4 vPos = float4(In.vpos.xy - 0.5, In.vpos.zw);");
        // vFace is a signed float in D3D9 (positive front-facing) but a bool
        // in D3D11. Shaders compare it against zero or use its sign, so it is
        // rebuilt as a float4 rather than exposed as the bool.
        if (m_out.usesVFace) {
            Line("float _face = In.isFront ? 1.0 : -1.0;");
            Line("float4 vFace = _face.xxxx;");
        }
        EmitTempDecls();
        EmitBody();

        // ps_1_x has no output register: the shader's result is whatever is in
        // r0 when it ends.
        if (m_v.sm1()) Line("oC0 = r0;");

        // Alpha test and fog were render states in D3D9 and have no
        // equivalent in D3D11 or D3D12, so they are appended to every pixel
        // shader and selected at compile time by macro. Both operate on oC0
        // after the shader body has finished with it, in that order — which is
        // also why a shader mod that edits the final colour belongs directly
        // above this block, and why editing oC0.a here changes what the alpha
        // test sees.

        m_s +=
            "#if DX9_ATEST == 1\n"
            "    discard;                                          // NEVER\n"
            "#elif DX9_ATEST == 2\n"
            "    if (!(oC0.a <  g_emuMisc.z)) discard;             // LESS\n"
            "#elif DX9_ATEST == 3\n"
            "    if (!(abs(oC0.a - g_emuMisc.z) < 0.00196)) discard; // EQUAL\n"
            "#elif DX9_ATEST == 4\n"
            "    if (!(oC0.a <= g_emuMisc.z)) discard;             // LESSEQUAL\n"
            "#elif DX9_ATEST == 5\n"
            "    if (!(oC0.a >  g_emuMisc.z)) discard;             // GREATER\n"
            "#elif DX9_ATEST == 6\n"
            "    if (!(abs(oC0.a - g_emuMisc.z) >= 0.00196)) discard; // NOTEQUAL\n"
            "#elif DX9_ATEST == 7\n"
            "    if (!(oC0.a >= g_emuMisc.z)) discard;             // GREATEREQUAL\n"
            "#endif\n";

        {
            const char* w = m_out.usesVPos ? "In.vpos.w" : "_fogSvPos.w";

            const std::string vfog = (!m_v.sm3() && hasIn)
                ? "    oC0.rgb = lerp(g_fogColor.rgb, oC0.rgb, saturate(In.fog)); // VERTEX\n"
                : "    /* vertex fog N/A here (SM3 / input-less PS) */\n";
            m_s += std::string() +
                "#if DX9_FOG == 3\n"
                "    { float _ff = saturate((g_fogParams.y - " + w + ") / "
                         "(g_fogParams.y - g_fogParams.x + 1e-6));"
                "  oC0.rgb = lerp(g_fogColor.rgb, oC0.rgb, _ff); }   // LINEAR\n"
                "#elif DX9_FOG == 1\n"
                "    { float _ff = saturate(exp(-g_fogParams.z * " + w + "));"
                "  oC0.rgb = lerp(g_fogColor.rgb, oC0.rgb, _ff); }   // EXP\n"
                "#elif DX9_FOG == 2\n"
                "    { float _fe = g_fogParams.z * " + w + ";"
                "  float _ff = saturate(exp(-_fe * _fe));"
                "  oC0.rgb = lerp(g_fogColor.rgb, oC0.rgb, _ff); }   // EXP2\n"
                "#elif DX9_FOG == 4\n"
                + vfog +
                "#endif\n";
        }

        if (mrt) {
            Line("PSOut _O = (PSOut)0;");
            for (int i = 0; i < 4; ++i)
                if (m_oCMask & (1u << i))
                    Line("_O.c" + std::to_string(i) + " = oC" + std::to_string(i) + ";");
            if (m_out.writesDepth) Line("_O.depth = oDepth;");
            Line("return _O;");
        } else {
            Line("return oC0;");
        }
        m_s += "}\n";
        s = std::move(m_s);
        m_s.clear();
    }
};

}

// Returns the byte length of the shader beginning at `tokens`, or 0.
//
// D3D9's CreateVertexShader and CreatePixelShader take a pointer with no
// length: the caller is trusted to pass a well-formed blob, and the runtime
// reads until the end token. We need a real length — to hash the shader for
// the caches, and to bound the parser — so the stream is walked once here
// without decoding it.
//
// From sm2 onward the instruction length is a field in the opcode token, which
// makes the walk trivial. sm1 has no such field, so lengths come from the
// per-opcode operand table instead. An opcode missing from that table falls
// back to scanning forward for the end token: less precise, but it keeps a
// shader we could still translate from being rejected here.
size_t MeasureD3D9Shader(const uint32_t* tokens, size_t maxBytes)
{
    if (!tokens || maxBytes < 8) return 0;
    const size_t n = maxBytes / 4;
    const uint32_t vt = tokens[0];
    const uint32_t hi = vt >> 16;
    if (hi != 0xFFFE && hi != 0xFFFF) return 0;

    Version v{};
    v.ps = (hi == 0xFFFF);
    v.major = (vt >> 8) & 0xFF;
    v.minor = vt & 0xFF;

    size_t pos = 1;
    while (pos < n) {
        const uint32_t tok = tokens[pos];
        if (tok == 0x0000FFFFu) return (pos + 1) * 4;
        if ((tok & 0xFFFFu) == OP_COMMENT && !(tok & 0x80000000u)) {
            pos += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        if (v.major >= 2) {
            pos += 1 + ((tok >> 24) & 0xF);
            continue;
        }

        const uint16_t op = static_cast<uint16_t>(tok & 0xFFFF);
        size_t operands = 0;
        if (op == OP_DEF)       operands = 5;
        else if (op == OP_DEFI) operands = 5;
        else if (op == OP_DEFB) operands = 2;
        else if (op == OP_DCL)  operands = 2;
        else {
            const OpCount oc = OperandCounts(op, v);
            if (oc.nsrc < 0) {

                for (size_t p = pos + 1; p < n; ++p)
                    if (tokens[p] == 0x0000FFFFu) return (p + 1) * 4;
                return 0;
            }
            operands = static_cast<size_t>(oc.hasDst) + static_cast<size_t>(oc.nsrc);
        }
        pos += 1 + operands;
    }
    return 0;
}

bool TranslateD3D9Shader(
    const uint32_t*            tokens,
    size_t                     maxBytes,
    const Sm3TranslateOptions& opt,
    Sm3TranslateResult&        out)
{
    out = Sm3TranslateResult{};
    if (!tokens || maxBytes < 8) { out.error = "null/short blob"; return false; }

    Version ver{};
    std::vector<Inst> ir;
    ir.reserve(256);
    Parser parser(tokens, maxBytes / 4);
    if (!parser.Parse(ver, ir, out.error)) return false;

    Emitter em(ver, ir, opt, out);
    if (!em.Run()) { out.error = em.TakeError(); if (out.error.empty()) out.error = "emit failed"; return false; }
    return true;
}

}
