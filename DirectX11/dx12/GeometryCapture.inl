#include <atomic>
#include <map>
#include <tuple>
#include <wrl/client.h>
#include "Dx12CommandQueue.h"

namespace {
const UINT64 GEO_RANGE_CAP = 32ull << 20, GEO_TOTAL_CAP = 128ull << 20;
const size_t GEO_ROWS_CAP = 16384;
struct GeometrySubmission {
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	UINT64 order = 0;
	unsigned count = 0;
};
struct GeometryView {
	ID3D12Resource *resource = nullptr;
	UINT64 serial = 0, offset = 0, version = 0, writer_order = 0;
	ID3D12CommandQueue *writer = nullptr;
	const char *record_status = nullptr;
	const char *queue_proof = nullptr;
	UINT bytes = 0, slot = 0, stride = 0, format = 0, crc = 0;
	uint32_t hash = 0;
	bool ib = false;
	const char *status = nullptr;
	size_t copy = SIZE_MAX;
};
struct GeometryDraw {
	UINT64 recording;
	UINT draw, count, start, instances, first_instance, topology;
	INT base_vertex;
	std::vector<Dx12InputElement> layout;
	std::vector<GeometryView> views;
};
struct GeometryCopy {
	const GeometryView *view;
	ID3D12Resource *readback = nullptr;
	const char *status = nullptr;
	wchar_t name[40] = {};
};
static SRWLOCK geometry_submit = SRWLOCK_INIT;
static SRWLOCK geometry_events = SRWLOCK_INIT;
static std::atomic<bool> geometry_accepting(false);
static UINT64 geometry_serial;
static bool geometry_unknown_lists;
static std::unordered_map<UINT64, GeometrySubmission> geometry_submissions;
static std::vector<GeometryDraw> geometry_draws;
static size_t geometry_rows, geometry_omitted;

static void remember_geometry_view(GeometryDraw &draw, Dx12GraphicsCommandList *list,
		bool ib, UINT slot, UINT64 va, UINT bytes, UINT stride, UINT format, uint32_t hash)
{
	if (geometry_rows >= GEO_ROWS_CAP) {
		geometry_omitted++;
		return;
	}
	geometry_rows++;
	GeometryView v;
	v.ib = ib;
	v.slot = slot;
	v.stride = stride;
	v.format = format;
	v.bytes = bytes;
	v.hash = hash;
	Dx12ResourceInfo info;
	ID3D12Resource *resource = nullptr;
	UINT item = format == DXGI_FORMAT_R16_UINT ? 2 : format == DXGI_FORMAT_R32_UINT ? 4 : 0;
	if (!va || !bytes)
		v.status = "unbound";
	else if ((ib && (!item || va % item || bytes % item)) || (!ib && !stride))
		v.status = "invalid_view";
	else if (ib && ((UINT64)draw.start + draw.count > bytes / item))
		v.status = "draw_range_out_of_bounds";
	else if (bytes > GEO_RANGE_CAP)
		v.status = "view_budget";
	else if (!Dx12FindBuffer(va, &resource, &v.offset) || !Dx12GetResourceInfo(resource, &info))
		v.status = "untracked_resource";
	else if (info.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
			v.offset > info.desc.Width || bytes > info.desc.Width - v.offset)
		v.status = "resource_range_out_of_bounds";
	else {
		v.serial = info.serial;
		if (info.geometry_barrier_unproven)
			v.status = "excluded_barrier";
		else if (!info.geometry_safe_committed || info.heap_type != D3D12_HEAP_TYPE_DEFAULT ||
				info.cpu_accessible || info.desc.Flags != D3D12_RESOURCE_FLAG_NONE)
			v.status = "unsupported_heap_or_flags";
		else if (Dx12ListWroteBuffer(&list->mBufferWrites, resource, v.offset, bytes) ||
				!Dx12BufferGeometryVersion(resource, v.offset, bytes, &v.version, &v.crc, &v.writer, &v.writer_order))
			v.status = "unstable_upload";
		else {
			resource->AddRef();
			v.resource = resource;
		}
	}
	v.record_status = v.status;
	draw.views.push_back(v);
}

static void remember_geometry(Dx12GraphicsCommandList *list, const Dx12PipelineInfo *pipeline,
		bool indexed, UINT count, UINT instances, UINT start, INT base, UINT first_instance, UINT draw)
{
	auto options = G->cur_analyse_options;
	if (!(options & FrameAnalysisOptions::FMT_BUF_BIN) ||
			(!(options & FrameAnalysisOptions::DUMP_VB) && !(options & FrameAnalysisOptions::DUMP_IB)))
		return;
	if (geometry_rows >= GEO_ROWS_CAP) {
		geometry_omitted++;
		return;
	}
	if (list->mOrig->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
		return;
	bool want_ib = indexed && (options & FrameAnalysisOptions::DUMP_IB);
	UINT slots = options & FrameAnalysisOptions::DUMP_VB ?
			(pipeline->vb_reads_known ? pipeline->vb_read_slots : pipeline->vb_slots) : 0;
	if (!want_ib && !slots)
		return;

	if (!list->mFrameGeometryId)
		list->mFrameGeometryId = ++geometry_serial;
	AcquireSRWLockExclusive(&geometry_events);
	geometry_submissions.emplace(list->mFrameGeometryId, GeometrySubmission{});
	geometry_accepting.store(true, std::memory_order_release);
	ReleaseSRWLockExclusive(&geometry_events);
	GeometryDraw entry = {};
	entry.recording = list->mFrameGeometryId;
	entry.draw = draw;
	entry.count = count;
	entry.start = start;
	entry.instances = instances;
	entry.first_instance = first_instance;
	entry.topology = list->mState.topology;
	entry.base_vertex = base;
	entry.layout = pipeline->input_layout;
	if (want_ib) {
		DXGI_FORMAT ignored;
		const auto &view = list->mState.index_buffer;
		remember_geometry_view(entry, list, true, 0, view.BufferLocation, view.SizeInBytes, 0,
				view.Format, hash_of(Dx12ScopeIndexBuffer(&ignored)));
	}
	unsigned long slot;
	for (; _BitScanForward(&slot, slots); slots &= slots - 1) {
		if (slot >= ARRAYSIZE(list->mState.vertex_buffers))
			continue;
		UINT ignored;
		const auto &view = list->mState.vertex_buffers[slot];
		remember_geometry_view(entry, list, false, slot, view.BufferLocation, view.SizeInBytes,
				view.StrideInBytes, 0, hash_of(Dx12ScopeVertexBuffer(slot, &ignored)));
	}
	geometry_draws.push_back(std::move(entry));
}

static void json_string(FILE *out, const std::string &value)
{
	fputc('"', out);
	for (unsigned char ch : value) {
		if (ch == '\\' || ch == '"')
			fprintf(out, "\\%c", ch);
		else if (ch < 32 || ch >= 127)
			fprintf(out, "\\u%04x", (unsigned)ch);
		else
			fputc(ch, out);
	}
	fputc('"', out);
}

static bool geometry_version_ok(const GeometryView &v)
{
	UINT64 version, writer_order;
	UINT32 crc;
	ID3D12CommandQueue *writer;
	return Dx12BufferGeometryVersion(v.resource, v.offset, v.bytes, &version, &crc, &writer, &writer_order)
		&& version == v.version && crc == v.crc && writer == v.writer && writer_order == v.writer_order;
}

struct GeometrySignal {
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
};
// At most eight outstanding signals. Timeouts retain the objects until the
// fence advances (or the device is lost); no pending GPU event uses our stack.
static std::vector<GeometrySignal> geometry_signals;
static void retire_geometry_signals()
{
	for (size_t i = geometry_signals.size(); i; --i)
		if (geometry_signals[i-1].fence->GetCompletedValue() >= 1)
			geometry_signals.erase(geometry_signals.begin() + i - 1);
}

static bool same_geometry_device_node(ID3D12CommandQueue *queue)
{
	Microsoft::WRL::ComPtr<IUnknown> a, b;
	if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
			FAILED(queue->GetDevice(IID_PPV_ARGS(&a))) ||
			FAILED(dx12_shim->device->QueryInterface(IID_PPV_ARGS(&b))) || a.Get() != b.Get())
		return false;
	UINT from = queue->GetDesc().NodeMask, to = dx12_shim->queue->GetDesc().NodeMask;
	// The staging allocations use node 0 (mask 1), as before.
	return (!from || from == 1) && (!to || to == 1);
}

// A CPU completion proof, not a Wait inserted into the game's queues. The
// caller holds the exclusive submit gate: all witnessed Execute calls are
// already issued, and wrapped writers cannot start a new Execute meanwhile.
static const char *geometry_draw_fence(ID3D12CommandQueue *queue, ULONGLONG deadline)
{
	retire_geometry_signals();
	if (geometry_signals.size() >= 8)
		return "draw_queue_signal_limit";
	if (GetTickCount64() >= deadline)
		return "draw_queue_timeout";
	GeometrySignal signal;
	signal.queue = queue;
	if (FAILED(dx12_shim->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&signal.fence))))
		return "draw_queue_fence_create_failed";
	Dx12QueueLock(queue);
	HRESULT hr = queue->Signal(signal.fence.Get(), 1);
	Dx12QueueUnlock(queue);
	if (FAILED(hr))
		return "draw_queue_signal_failed";
	ID3D12Fence *f = signal.fence.Get();
	geometry_signals.push_back(std::move(signal));
	for (;;) {
		const UINT64 value = f->GetCompletedValue();
		if (value == UINT64_MAX)
			return "draw_queue_device_removed";
		if (value >= 1)
			return nullptr;
		if (GetTickCount64() >= deadline)
			return "draw_queue_timeout";
		Sleep(1);
	}
}

static const char *geometry_queue_check(GeometryView &view, const GeometrySubmission &sub,
		std::map<ID3D12CommandQueue*, const char*> &fallback, ULONGLONG deadline)
{
	ID3D12CommandQueue *draw = sub.queue.Get(), *reader = dx12_shim->queue;
	if (!draw || !reader || !sub.order || FAILED(dx12_shim->device->GetDeviceRemovedReason()))
		return "draw_queue_unproven";
	if (draw == reader)
		view.queue_proof = "same_queue";
	else {
		if (!same_geometry_device_node(draw))
			return "draw_queue_device_or_node";
		if (Dx12QueueAfter(reader, draw, sub.order))
			view.queue_proof = "tracked_dependency";
		else {
			auto proof = fallback.find(draw);
			if (proof == fallback.end())
				proof = fallback.emplace(draw, geometry_draw_fence(draw, deadline)).first;
			if (proof->second)
				return proof->second;
			view.queue_proof = "draw_fence";
		}
	}
	if (view.writer == reader)
		return nullptr;
	// The draw completion/dependency also covers an earlier upload on the
	// same queue. A third queue needs independent tracked fence evidence.
	if (view.writer == draw && view.writer_order < sub.order)
		return nullptr;
	if (!view.writer || !Dx12QueueAfter(reader, view.writer, view.writer_order))
		return "upload_queue_unordered";
	return nullptr;
}

static void record_geometry_copy(GeometryCopy &copy)
{
	const GeometryView &v = *copy.view;
	if (!Dx12ShimReadGameResource(v.resource)) {
		copy.status = "not_resident";
		return;
	}
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC d = {};
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = v.bytes;
	d.Height = 1;
	d.DepthOrArraySize = d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy.readback)))) {
		copy.status = "readback_allocation_failed";
		return;
	}
	// This is a separate ExecuteCommandLists batch on the same queue. Buffer
	// states decay to COMMON at batch completion and can promote to COPY_SOURCE.
	// Do not add a guessed StateBefore or change the game's command lists.
	Dx12ShimList()->CopyBufferRegion(copy.readback, 0, v.resource, v.offset, v.bytes);
	Dx12ShimDeferRelease(v.resource);
	Dx12ShimDeferRelease(copy.readback);
}

static bool write_geometry_file(const wchar_t *path, const void *data, size_t bytes)
{
	std::wstring temporary = std::wstring(path) + L".tmp";
	FILE *out = nullptr;
	wfopen_ensuring_access(&out, temporary.c_str(), L"wb");
	if (!out)
		return false;
	bool ok = fwrite(data, 1, bytes, out) == bytes;
	if (fclose(out))
		ok = false;
	if (ok)
		ok = MoveFileExW(temporary.c_str(), path, MOVEFILE_WRITE_THROUGH) != 0;
	if (!ok)
		DeleteFileW(temporary.c_str());
	return ok;
}

static void finish_geometry()
{
	if (!geometry_accepting.load(std::memory_order_acquire))
		return;
	wchar_t directory[MAX_PATH], path[MAX_PATH];
	FILE *out = nullptr;
	if (swprintf_s(directory, L"%ls\\geometry", G->ANALYSIS_PATH) < 0 ||
			swprintf_s(path, L"%ls\\geometry.jsonl", G->ANALYSIS_PATH) < 0)
		path[0] = 0;
	if (path[0])
		wfopen_ensuring_access(&out, path, L"w");
	if (out)
		CreateDirectoryEnsuringAccess(directory);
	std::vector<GeometryCopy> copies;
	std::unordered_map<UINT64, GeometrySubmission> submissions;
	{
		// Hold the capture submit gate only while gathering and waiting for
		// the GPU copies. No game copy writer can be handed to a queue here.
		Dx12GeometrySubmissionLock lock(true, true);
		UINT64 budget = 0;
		const ULONGLONG queue_deadline = GetTickCount64() + 125;
		std::map<ID3D12CommandQueue*, const char*> queue_fallback;
		std::map<std::tuple<UINT64, UINT64, UINT, UINT64>, size_t> dedup;
		for (auto &draw : geometry_draws) {
			const auto &submitted = geometry_submissions.at(draw.recording);
			for (auto &v : draw.views) {
				if (!submitted.count)
					v.status = "unsubmitted";
				else if (submitted.count > 1)
					v.status = "repeat_submission";
				else if (geometry_unknown_lists)
					v.status = "untracked_submission";
				if (v.status || !out)
					continue;
				Dx12ResourceInfo info;
				if (!Dx12GetResourceInfo(v.resource, &info) || info.serial != v.serial)
					v.status = "resource_identity_changed";
				else if (info.geometry_barrier_unproven)
					v.status = "excluded_barrier";
				else if (info.state == DX12_STATE_UNKNOWN ||
						(info.state & ~(D3D12_RESOURCE_STATE_INDEX_BUFFER |
							D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
							D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
							D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
							D3D12_RESOURCE_STATE_COPY_SOURCE)))
					v.status = "unknown_or_unsafe_state";
				else if (!geometry_version_ok(v))
					v.status = "changed";
				else
					v.status = geometry_queue_check(v, submitted, queue_fallback, queue_deadline);
				if (v.status)
					continue;
				const auto key = std::make_tuple(v.serial, v.offset, v.bytes, v.version);
				auto same = dedup.find(key);
				if (same != dedup.end()) {
					v.copy = same->second;
					continue;
				}
				if (v.bytes > GEO_TOTAL_CAP - budget) {
					v.status = "frame_budget";
					continue;
				}
				v.copy = copies.size();
				dedup.emplace(key, v.copy);
				copies.push_back({ &v });
				auto &copy = copies.back();
				swprintf_s(copy.name, L"%06Iu.buf", v.copy + 1);
				record_geometry_copy(copy);
				if (copy.readback)
					budget += v.bytes;
			}
		}
		if (budget) {
			const UINT64 fence = Dx12ShimPendingFence();
			Dx12ShimSubmitAndWait();
			const bool done = Dx12ShimFenceReached(fence) && !Dx12ShimSubmissionDiscarded(fence)
					&& SUCCEEDED(dx12_shim->device->GetDeviceRemovedReason());
			for (auto &c : copies) {
				if (!c.readback)
					continue;
				if (!done)
					c.status = "readback_fence_failed";
				else if (!geometry_version_ok(*c.view))
					c.status = "changed";
			}
		}
		submissions.swap(geometry_submissions);
		geometry_unknown_lists = false;
		geometry_accepting.store(false, std::memory_order_release);
	}
	// No game submission is held for mapping or file IO. The readback is
	// immutable after the fence; compare its CRC with the verified upload.
	for (auto &c : copies) {
		if (!c.readback)
			continue;
		if (!c.status) {
			const auto &v = *c.view;
			const D3D12_RANGE range = { 0, v.bytes }, written = {};
			void *data = nullptr;
			HRESULT hr = c.readback->Map(0, &range, &data);
			if (FAILED(hr) || !data)
				c.status = "readback_map_failed";
			else {
				if (Dx12Crc32cWriteCombined(0, static_cast<const BYTE*>(data), v.bytes) != v.crc)
					c.status = "content_does_not_match_upload";
				else if (swprintf_s(path, L"%ls\\%ls", directory, c.name) < 0 ||
						!write_geometry_file(path, data, v.bytes))
					c.status = "write_failed";
				c.readback->Unmap(0, &written);
			}
		}
		c.readback->Release();
	}
	if (out) {
		for (const auto &d : geometry_draws) {
			const auto &sub = submissions.at(d.recording);
			for (const auto &v : d.views) {
				const GeometryCopy *c = v.copy == SIZE_MAX ? nullptr : &copies[v.copy];
				const char *status = v.status ? v.status : c ? c->status : "not_copied";
				fprintf(out, "{\"schema\":1,\"snapshot\":\"present\",\"draw\":%u,\"kind\":\"%s\",\"slot\":%u,"
					"\"resource_serial\":%llu,\"offset\":%llu,\"bytes\":%u,\"stride\":%u,\"index_format\":%u,"
					"\"binding_hash\":\"%08x\",\"topology\":%u,\"start\":%u,\"count\":%u,"
					"\"base_vertex\":%d,\"instances\":%u,\"first_instance\":%u,\"status\":\"%s\",",
					d.draw, v.ib ? "ib" : "vb", v.slot, v.serial, v.offset, v.bytes, v.stride, v.format,
					v.hash, d.topology, d.start, d.count, d.base_vertex, d.instances, d.first_instance, status ? status : "ok");
				fprintf(out, "\"draw_queue\":\"%p\",\"copy_queue\":\"%p\",\"writer_queue\":\"%p\","
					"\"draw_order\":%llu,\"writer_order\":%llu,\"queue_proof\":\"%s\",\"record_status\":\"%s\",",
					sub.queue.Get(), dx12_shim->queue, v.writer, sub.order, v.writer_order,
					v.queue_proof ? v.queue_proof : "none", v.record_status ? v.record_status : "eligible");
				if (!status && c)
					fprintf(out, "\"file\":\"geometry/%ls\",", c->name);
				else
					fputs("\"file\":null,", out);
				fputs("\"layout\":[", out);
				bool comma = false;
				for (const auto &el : d.layout) {
					if (comma) fputc(',', out);
					fputs("{\"semantic\":", out);
					json_string(out, el.semantic);
					fprintf(out, ",\"index\":%u,\"format\":%u,\"slot\":%u,\"offset\":%u,\"per_instance\":%s,\"step_rate\":%u}",
						el.index, el.format, el.slot, el.offset, el.per_instance ? "true" : "false", el.step_rate);
					comma = true;
				}
				fputs("]}\n", out);
			}
		}
		fclose(out);
	}
	if (log_file)
		fprintf(log_file, "\nGeometry snapshot: %Iu bindings, %Iu unique attempts, %Iu omitted by record cap; see geometry.jsonl%s\n",
				geometry_rows, copies.size(), geometry_omitted, out ? "" : " (manifest open failed)");
	for (auto &d : geometry_draws)
		for (auto &v : d.views)
			if (v.resource)
				v.resource->Release();
	geometry_draws.clear();
	geometry_rows = geometry_omitted = 0;
}
} // namespace

Dx12GeometrySubmissionLock::Dx12GeometrySubmissionLock(bool lock, bool exclusive) : held(lock), exclusive(exclusive)
{
	if (held && exclusive)
		AcquireSRWLockExclusive(&geometry_submit);
	else if (held)
		AcquireSRWLockShared(&geometry_submit);
}

Dx12GeometrySubmissionLock::~Dx12GeometrySubmissionLock()
{
	if (held && exclusive)
		ReleaseSRWLockExclusive(&geometry_submit);
	else if (held)
		ReleaseSRWLockShared(&geometry_submit);
}

bool Dx12FrameGeometryAccepting()
{
	return geometry_accepting.load(std::memory_order_acquire);
}

void Dx12FrameGeometryExecuted(Dx12GraphicsCommandList *list, ID3D12CommandQueue *queue, UINT64 order)
{
	if (!Dx12FrameGeometryAccepting() || (list && !list->mFrameGeometryId))
		return;
	AcquireSRWLockExclusive(&geometry_events);
	if (!list) {
		geometry_unknown_lists = true;
		ReleaseSRWLockExclusive(&geometry_events);
		return;
	}
	auto it = geometry_submissions.find(list->mFrameGeometryId);
	if (it != geometry_submissions.end()) {
		if (it->second.count == 0) {
			it->second.queue = queue;
			it->second.order = order;
		}
		if (it->second.count < 2)
			it->second.count++;
	}
	ReleaseSRWLockExclusive(&geometry_events);
}
