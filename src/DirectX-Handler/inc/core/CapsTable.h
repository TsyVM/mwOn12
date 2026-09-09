// Builds the D3DCAPS9 structure returned by GetDeviceCaps.
//
// The single place the answer is assembled, so the factory and the device
// cannot drift apart. See the implementation for why the contents of this
// structure deserve more care than they appear to: an under-reported
// capability makes a game silently abandon a feature, with no error raised
// anywhere.

#pragma once

#ifndef MWON12_CAPS_TABLE_H
#define MWON12_CAPS_TABLE_H

#include <d3d9.h>

namespace mwon12 {

void SynthesiseCaps(UINT adapterOrdinal, D3DCAPS9* pCaps) noexcept;

}

#endif
