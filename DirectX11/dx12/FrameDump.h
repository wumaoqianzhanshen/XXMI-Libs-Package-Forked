#pragma once

#include "PipelineRegistry.h"

class Dx12GraphicsCommandList;

// Frame analysis for D3D12, as far as it goes: for the frame after the
// analyse_frame key, log.txt in the FrameAnalysis folder lists the draws with
// the hashes of what they use, and with dump_tex in analyse_options the
// textures they read are written next to it under the names D3D11 frame
// analysis gives them.
//
// A draw is seen when the game records it, which is before anything of it has
// been executed: there is nothing in its render targets to write yet, and the
// state its textures are in is only known for what has been executed. So the
// textures are remembered, and copied by a list of ours when the frame is
// presented. They are written as they are by then, the top mip of the first
// array slice of each.
//
// Not dumped: render targets, buffers, dispatches, and what the mods draw.

// For a draw of the game with its scope open, during a frame being analysed.
// With the lock of Bridge.h held, as for the next.
void Dx12FrameDumpDraw(Dx12GraphicsCommandList *list, const Dx12PipelineInfo *pipeline, bool indexed, UINT count,
		UINT instances, UINT start, INT base_vertex, UINT start_instance);

// Once per present, after what the frame recorded on our list was submitted
void Dx12FrameDumpFramePresented();

// Keep a newly requested capture armed across a few Presents with no recorded draw.
void Dx12FrameDumpAdvanceCaptureWindow();

// Submission hook only: no bridge/queue work while taking this gate.
class Dx12GeometrySubmissionLock {
	bool held, exclusive;
public:
	explicit Dx12GeometrySubmissionLock(bool lock, bool exclusive = false);
	~Dx12GeometrySubmissionLock();
	Dx12GeometrySubmissionLock(const Dx12GeometrySubmissionLock&) = delete;
	Dx12GeometrySubmissionLock& operator=(const Dx12GeometrySubmissionLock&) = delete;
};
bool Dx12FrameGeometryAccepting();
void Dx12FrameGeometryExecuted(Dx12GraphicsCommandList *list, ID3D12CommandQueue *queue, UINT64 order);
