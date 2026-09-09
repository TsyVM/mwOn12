// Implements D3D9's SetPrivateData / GetPrivateData / FreePrivateData.
//
// D3D9 lets an application attach arbitrary blobs to a resource, keyed by
// GUID. Games use it to hang their own bookkeeping off objects the runtime
// owns. It is a small feature and easy to dismiss, but a game that stores
// something and cannot read it back may misbehave in ways that look nothing
// like a graphics fault.
//
// Blobs are copied rather than referenced, matching the D3D9 contract, and
// released with the object.

#pragma once

#ifndef MWON12_DX12_PRIVATEDATA12_H
#define MWON12_DX12_PRIVATEDATA12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <map>
#include <vector>

namespace mwon12 {

class PrivateDataStore {
public:
    PrivateDataStore() = default;
    ~PrivateDataStore() { Clear(); }

    PrivateDataStore(const PrivateDataStore&)            = delete;
    PrivateDataStore& operator=(const PrivateDataStore&) = delete;

    HRESULT Set(REFGUID guid, const void* data, DWORD size, DWORD flags) noexcept
    {
        if (!data && size) return D3DERR_INVALIDCALL;
        Free(guid);
        Entry e;
        if (flags & D3DSPD_IUNKNOWN) {
            if (size != sizeof(IUnknown*)) return D3DERR_INVALIDCALL;
            e.unknown = *static_cast<IUnknown* const*>(data);
            if (e.unknown) e.unknown->AddRef();
        } else {
            e.bytes.resize(size);
            if (size) std::memcpy(e.bytes.data(), data, size);
        }
        m_map.emplace(Key{ guid }, std::move(e));
        return D3D_OK;
    }

    HRESULT Get(REFGUID guid, void* data, DWORD* pSize) const noexcept
    {
        if (!pSize) return D3DERR_INVALIDCALL;
        const auto it = m_map.find(Key{ guid });
        if (it == m_map.end()) return D3DERR_NOTFOUND;

        const DWORD needed = it->second.unknown
                           ? DWORD(sizeof(IUnknown*))
                           : DWORD(it->second.bytes.size());
        if (!data) { *pSize = needed; return D3D_OK; }
        if (*pSize < needed) { *pSize = needed; return D3DERR_MOREDATA; }

        if (it->second.unknown) {
            it->second.unknown->AddRef();
            *static_cast<IUnknown**>(data) = it->second.unknown;
        } else if (needed) {
            std::memcpy(data, it->second.bytes.data(), needed);
        }
        *pSize = needed;
        return D3D_OK;
    }

    HRESULT Free(REFGUID guid) noexcept
    {
        const auto it = m_map.find(Key{ guid });
        if (it == m_map.end()) return D3DERR_NOTFOUND;
        if (it->second.unknown) it->second.unknown->Release();
        m_map.erase(it);
        return D3D_OK;
    }

    void Clear() noexcept
    {
        for (auto& kv : m_map)
            if (kv.second.unknown) kv.second.unknown->Release();
        m_map.clear();
    }

private:
    struct Key {
        GUID g;
        bool operator<(const Key& o) const noexcept
        {
            return std::memcmp(&g, &o.g, sizeof(GUID)) < 0;
        }
    };
    struct Entry {
        std::vector<BYTE> bytes;
        IUnknown*         unknown{ nullptr };
    };
    std::map<Key, Entry> m_map;
};

}

#endif
