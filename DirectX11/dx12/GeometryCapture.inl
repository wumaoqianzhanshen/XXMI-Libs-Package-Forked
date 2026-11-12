#include <atomic>
#include <map>
#include <tuple>

namespace {
const UINT64 GEO_RANGE_CAP = 32ull << 20, GEO_TOTAL_CAP = 128ull << 20;
const size_t GEO_ROWS_CAP = 16384;
struct GeometrySubmission {
	ID3D12CommandQueue *queue = nullptr;
	unsigned count = 0;
};
struct GeometryView {
	ID3D12Resource *resource = nullptr;
	UINT64 serial = 0, offset = 0, version = 0;
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
				!Dx12BufferGeometryVersion(resource, v.offset, bytes, dx12_shim->queue, &v.version, &v.crc))
			v.status = "unstable_upload";
		else {
			resource->AddRef();
			v.resource = resource;
		}
	}
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
	UINT64 version;
	UINT32 crc;
	return Dx12BufferGeometryVersion(v.resource, v.offset, v.bytes, dx12_shim->queue, &version, &crc)
		&& version == v.version && crc == v.crc;
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
	{
		// Hold the capture submit gate only while gathering and waiting for
		// the GPU copies. No game copy writer can be handed to a queue here.
		Dx12GeometrySubmissionLock lock(true, true);
		UINT64 budget = 0;
		std::map<std::tuple<UINT64, UINT64, UINT, UINT64>, size_t> dedup;
		for (auto &draw : geometry_draws) {
			const auto &submitted = geometry_submissions.at(draw.recording);
			for (auto &v : draw.views) {
				if (!submitted.count)
					v.status = "unsubmitted";
				else if (submitted.count > 1)
					v.status = "repeat_submission";
				else if (submitted.queue != dx12_shim->queue)
					v.status = "different_queue";
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
		geometry_submissions.clear();
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
			for (const auto &v : d.views) {
				const GeometryCopy *c = v.copy == SIZE_MAX ? nullptr : &copies[v.copy];
				const char *status = v.status ? v.status : c ? c->status : "not_copied";
				fprintf(out, "{\"schema\":1,\"snapshot\":\"present\",\"draw\":%u,\"kind\":\"%s\",\"slot\":%u,"
					"\"resource_serial\":%llu,\"offset\":%llu,\"bytes\":%u,\"stride\":%u,\"index_format\":%u,"
					"\"binding_hash\":\"%08x\",\"topology\":%u,\"start\":%u,\"count\":%u,"
					"\"base_vertex\":%d,\"instances\":%u,\"first_instance\":%u,\"status\":\"%s\",",
					d.draw, v.ib ? "ib" : "vb", v.slot, v.serial, v.offset, v.bytes, v.stride, v.format,
					v.hash, d.topology, d.start, d.count, d.base_vertex, d.instances, d.first_instance, status ? status : "ok");
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

void Dx12FrameGeometryExecuted(Dx12GraphicsCommandList *list, ID3D12CommandQueue *queue)
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
		if (it->second.count == 0)
			it->second.queue = queue;
		if (it->second.count < 2)
			it->second.count++;
	}
	ReleaseSRWLockExclusive(&geometry_events);
}
