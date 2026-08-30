#pragma once

#include "Probe.h"

#include <windows.h>

#include <string>

#include <winrt/base.h>

namespace xvr::probe {

// Appends to the detailed report. Kept in one place so every probe section
// produces consistently formatted output in the log.
void AddLine(Report& report, std::wstring line);

std::wstring HrText(HRESULT hr);

void ProbeDisplay(Report& report);
void ProbeD3D12Encode(Report& report);

} // namespace xvr::probe
