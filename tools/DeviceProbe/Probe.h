#pragma once

// One-shot capability probe for Xbox Dev Mode.
//
// Answers the questions a PC cannot answer for us:
//   - what the real app memory budget is on this console (and which memory mode)
//   - whether a hardware H.264/HEVC encoder MFT exists at all under Dev Mode UWP
//   - whether that encoder accepts the resolutions the stereo pipeline needs
//
// Everything it learns goes to the log file; a condensed version goes on screen.

#include <string>
#include <vector>

#include <d3d11.h>

namespace xvr::probe {

struct Report
{
    // Full detail, one line per fact. Written to the log.
    std::vector<std::wstring> lines;
    // A handful of headline results, rendered on the TV.
    std::vector<std::wstring> summary;
    bool hardwareH264Found = false;
};

Report Run(ID3D11Device* device);

} // namespace xvr::probe
