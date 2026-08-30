#pragma once

// Phase 1 step 2: stream encoded stereo frames to a client over UDP.
//
// Identical pipeline to the throughput test - only the sink changes, from writing files
// to sending packets. That substitution is the whole reason IEncodedFrameSink exists.

#include <string>
#include <vector>

#include <d3d11_4.h>

namespace sample {

class StreamSession
{
public:
    // Brings up the pipeline and starts listening for a client. Does not block.
    bool Start(ID3D11Device* device, ID3D11DeviceContext* context);

    // Renders and streams one frame if a client is connected, otherwise polls for one.
    // Returns lines describing current state, for the on-screen display.
    void Tick(std::vector<std::wstring>& headline, std::vector<std::wstring>& detail);

    void Stop();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace sample
