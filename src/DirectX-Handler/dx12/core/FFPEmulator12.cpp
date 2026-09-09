// Compiles, caches and binds the generated fixed-function shaders under D3D12.
//
// The permutation key drives the shared generator in core/FFPShaderGen, so an
// identical key always yields identical HLSL. What is D3D12-specific is only
// the compile target and owning the constant buffer as a D3D12 resource
// updated through the per-frame upload allocator.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/FFPEmulator12.h>
#include <core/DeviceContext12.h>
#include <core/ShaderMods.h>
#include <core/Log.h>

#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <wrl/client.h>
#include <cstring>
#include <new>
#include <string>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

namespace {

HRESULT CompileOne(const std::string& src, bool isVertexShader,
                   CompiledShader12& out) noexcept
{
    ComPtr<ID3DBlob> blob, errs;
    const HRESULT hr = D3DCompile(
        src.data(), src.size(), "mw4real_dx12_ffp",
        nullptr, nullptr, "main",
        isVertexShader ? "vs_5_0" : "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob.GetAddressOf(), errs.GetAddressOf());
    if (FAILED(hr)) {
        if (errs)
            DXLOG_ERROR("[dx12] fixed-function %s failed to compile: %s",
                        isVertexShader ? "VS" : "PS",
                        static_cast<const char*>(errs->GetBufferPointer()));
        DXLOG_ERROR("[dx12] generated fixed-function HLSL:\n%s", src.c_str());
        return hr;
    }

    out.isVertexShader = isVertexShader;
    out.dxbc.assign(static_cast<const uint8_t*>(blob->GetBufferPointer()),
                    static_cast<const uint8_t*>(blob->GetBufferPointer()) +
                        blob->GetBufferSize());

    if (!isVertexShader)
        return S_OK;

    ComPtr<ID3D12ShaderReflection> refl;
    if (FAILED(D3DReflect(out.dxbc.data(), out.dxbc.size(),
                          IID_PPV_ARGS(refl.GetAddressOf()))))
        return S_OK;

    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return S_OK;

    out.inputSignature.clear();
    out.inputSignature.reserve(sd.InputParameters);
    for (UINT i = 0; i < sd.InputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC p{};
        if (FAILED(refl->GetInputParameterDesc(i, &p)) || !p.SemanticName) continue;
        CompiledShader12::SigInput si;
        si.name  = p.SemanticName;
        si.index = p.SemanticIndex;
        out.inputSignature.push_back(std::move(si));
    }
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC p{};
        if (FAILED(refl->GetOutputParameterDesc(i, &p)) || !p.SemanticName) continue;
        if (_stricmp(p.SemanticName, "TEXCOORD") == 0 && p.SemanticIndex < 32)
            out.outTexMask |= (1u << p.SemanticIndex);
        else if (_stricmp(p.SemanticName, "COLOR") == 0)
            (p.SemanticIndex == 0 ? out.outColor0 : out.outColor1) = true;
        else if (_stricmp(p.SemanticName, "FOG") == 0)
            out.outFog = true;
    }
    return S_OK;
}

}

HRESULT FFPEmulator12::Compile(const FFPPermKey& key, FFPProgram12* out) noexcept
{
    const std::string vsSrc = GenerateFFPVS(key);
    const std::string psSrc = GenerateFFPPS(key);

    auto compileStage = [&](const std::string& src, bool isVertexShader,
                            CompiledShader12& out12) -> bool {
        if (shadermods::Active()) {
            const shadermods::Id id =
                shadermods::MakeFixedFunctionId(&key, sizeof(key), isVertexShader);
            shadermods::Dump(id, src);

            std::string modded, name;
            if (shadermods::Load(id, &modded, &name)) {
                if (SUCCEEDED(CompileOne(modded, isVertexShader, out12))) {
                    DXLOG_INFO("[shadermods] using %s.hlsl", name.c_str());
                    return true;
                }
                shadermods::ReportCompileFailure(name);
            }
        }
        return SUCCEEDED(CompileOne(src, isVertexShader, out12));
    };

    out->vsValid = compileStage(vsSrc, true,  out->vs);
    out->psValid = compileStage(psSrc, false, out->ps);

    if (!out->vsValid || !out->psValid) {
        DXLOG_ERROR("[dx12] fixed-function permutation failed to build "
                    "(vs=%d ps=%d, lights=%u stages=%u posT=%u)",
                    out->vsValid ? 1 : 0, out->psValid ? 1 : 0,
                    (unsigned)key.lightCount, (unsigned)key.activeStageCount,
                    (unsigned)key.positionType);
        return E_FAIL;
    }
    return S_OK;
}

HRESULT FFPEmulator12::GetOrCompile(const FFPPermKey& key,
                                    const FFPProgram12** ppOut) noexcept
{
    if (!ppOut) return D3DERR_INVALIDCALL;
    *ppOut = nullptr;

    {
        AcquireSRWLockShared(&m_lock);
        if (const auto it = m_cache.find(key); it != m_cache.end()) {
            *ppOut = it->second.get();
            ReleaseSRWLockShared(&m_lock);
            return S_OK;
        }
        ReleaseSRWLockShared(&m_lock);
    }

    auto prog = std::make_unique<FFPProgram12>();
    const HRESULT hr = Compile(key, prog.get());
    if (FAILED(hr)) return hr;

    AcquireSRWLockExclusive(&m_lock);
    auto [it, inserted] = m_cache.emplace(key, std::move(prog));
    *ppOut = it->second.get();
    const size_t n = m_cache.size();
    ReleaseSRWLockExclusive(&m_lock);

    DXLOG_TRACE("[dx12] fixed-function permutation compiled (%zu cached)", n);
    return S_OK;
}

size_t FFPEmulator12::PermutationCount() const noexcept
{
    AcquireSRWLockShared(&m_lock);
    const size_t n = m_cache.size();
    ReleaseSRWLockShared(&m_lock);
    return n;
}

}
