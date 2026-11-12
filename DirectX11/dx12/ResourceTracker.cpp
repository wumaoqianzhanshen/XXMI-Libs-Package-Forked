#include "ResourceTracker.h"

#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

#include "BufferHash.h"

// {5D0C2A8E-93B4-4F0E-9C57-3E1B7A64D2F1}
static const GUID DestructionNotifierGuid = { 0x5d0c2a8e, 0x93b4, 0x4f0e, { 0x9c, 0x57, 0x3e, 0x1b, 0x7a, 0x64, 0xd2, 0xf1 } };

// D3D12 objects release the interfaces stored as their private data when they
// are destroyed, which is the only notification of that there is (this is how
// ReShade tracks resource lifetime as well).
class DestructionNotifier final : public IUnknown
{
	volatile LONG mRef;
	void *mObject;
	void (*mCallback)(void *object);

public:
	DestructionNotifier(void *object, void (*callback)(void *object)) :
		mRef(1), mObject(object), mCallback(callback)
	{}

	void Disarm()
	{
		mCallback = NULL;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
	{
		if (!ppvObject)
			return E_POINTER;
		if (!IsEqualIID(riid, __uuidof(IUnknown))) {
			*ppvObject = NULL;
			return E_NOINTERFACE;
		}
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&mRef);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG ref = InterlockedDecrement(&mRef);

		if (!ref) {
			if (mCallback)
				mCallback(mObject);
			delete this;
		}
		return ref;
	}
};

bool Dx12NotifyDestruction(ID3D12Object *object, REFGUID guid, void (*callback)(void *context), void *context)
{
	DestructionNotifier *notifier = new DestructionNotifier(context, callback);
	bool armed = SUCCEEDED(object->SetPrivateDataInterface(guid, notifier));

	if (!armed)
		notifier->Disarm();
	notifier->Release();
	return armed;
}

void Dx12NotifyDestruction(ID3D12Object *object, void (*callback)(void *object))
{
	Dx12NotifyDestruction(object, DestructionNotifierGuid, callback, object);
}

static SRWLOCK gone_lock = SRWLOCK_INIT;
static std::unordered_set<const void*> gone;

void Dx12KeepGone(void *storage)
{
	AcquireSRWLockExclusive(&gone_lock);
	gone.insert(storage);
	ReleaseSRWLockExclusive(&gone_lock);
}

bool Dx12IsGone(const void *object)
{
	bool result;

	AcquireSRWLockShared(&gone_lock);
	result = gone.count(object) != 0;
	ReleaseSRWLockShared(&gone_lock);
	return result;
}

struct ResourceShard {
	SRWLOCK lock;
	std::unordered_map<ID3D12Resource*, Dx12ResourceInfo> resources;
};

// Resources are created and destroyed from several threads at once while
// textures stream in, hence more than one lock.
static ResourceShard shards[64];

struct BufferRange {
	UINT64 end;
	ID3D12Resource *resource;
};

static SRWLOCK buffer_lock = SRWLOCK_INIT;
static std::map<UINT64, BufferRange> buffers;

// See Dx12FindBufferStamp. A counter per 64 KiB of address space, which is
// what D3D12 aligns buffers to, shared with whatever else hashes to it, and
// one for buffers covering too many of those. Engines create and destroy
// buffers all the time: counters that many buffers shared would have every
// one of them looked up again for each.
const UINT VA_STAMP_SHIFT = 16;
const UINT64 VA_STAMP_MAX_SPAN = 256;
static volatile LONG64 va_stamps[16384];
static volatile LONG64 va_stamp_all;

static volatile LONG64* va_stamp(UINT64 granule)
{
	return &va_stamps[(granule * 0x9e3779b97f4a7c15ull) >> 50];
}

// With buffer_lock held
static void stamp_buffer(UINT64 va, UINT64 end)
{
	UINT64 first = va >> VA_STAMP_SHIFT, last = (end > va ? end - 1 : va) >> VA_STAMP_SHIFT;

	if (last - first >= VA_STAMP_MAX_SPAN) {
		InterlockedIncrement64(&va_stamp_all);
		return;
	}
	for (; first <= last; first++)
		InterlockedIncrement64(va_stamp(first));
}

UINT64 Dx12FindBufferStamp(UINT64 va)
{
	return (UINT64)va_stamp_all + (UINT64)*va_stamp(va >> VA_STAMP_SHIFT);
}

void Dx12FindBufferStampCounters(UINT64 va, const volatile LONG64 *counters[2])
{
	counters[0] = &va_stamp_all;
	counters[1] = va_stamp(va >> VA_STAMP_SHIFT);
}

// Only explicitly observed, private DEFAULT heaps are eligible. Keep
// allocation footprints (not view widths) until the heap dies: even a
// destroyed/replaced allocation leaves its span ambiguous for capture.
namespace {
struct GeometryHeapSpan {
	UINT64 end, serial;
};
struct GeometryHeapHistory {
	ID3D12Device *device;
	UINT64 serial, size;
	bool excluded = false;
	std::map<UINT64, GeometryHeapSpan> spans;
};
const GUID geometry_heap_lifetime = { 0xb7670e64, 0x04ab, 0x4465, {0xa5,0x13,0xf6,0x3b,0x4f,0x0c,0x36,0x6f} };
const GUID geometry_backing_heap_pin = { 0xb4ee534e, 0x81f4, 0x49da, {0xb1,0x46,0x93,0xa9,0xa4,0x66,0x7b,0x0c} };
SRWLOCK geometry_heap_lock = SRWLOCK_INIT;
std::unordered_map<IUnknown*, GeometryHeapHistory> geometry_heaps;
UINT64 geometry_placement_serial;
constexpr size_t geometry_heap_limit = 1024;
constexpr size_t geometry_spans_per_heap = 4096;

void geometry_heap_destroyed(void *key)
{
	AcquireSRWLockExclusive(&geometry_heap_lock);
	geometry_heaps.erase(static_cast<IUnknown*>(key));
	ReleaseSRWLockExclusive(&geometry_heap_lock);
}

void exclude_geometry_heap(GeometryHeapHistory &heap)
{
	heap.excluded = true;
	heap.spans.clear();
}
}

void Dx12GeometryRegisterHeap(ID3D12Device *device, void **output)
{
	Microsoft::WRL::ComPtr<ID3D12Heap> heap;
	Microsoft::WRL::ComPtr<IUnknown> key;
	if (!output || !*output || FAILED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&heap)))
			|| FAILED(heap.As(&key)))
		return;
	const auto desc = heap->GetDesc();
	if (desc.Properties.Type != D3D12_HEAP_TYPE_DEFAULT || !desc.SizeInBytes ||
			(desc.Flags & ~(D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES)) ||
			device->GetNodeCount() != 1)
		return;
	// The caller holds the created heap alive until registration is complete.
	if (!Dx12NotifyDestruction(heap.Get(), geometry_heap_lifetime, geometry_heap_destroyed, key.Get()))
		return;
	AcquireSRWLockExclusive(&geometry_heap_lock);
	if (geometry_heaps.size() < geometry_heap_limit)
		geometry_heaps.emplace(key.Get(), GeometryHeapHistory{device, ++geometry_placement_serial, desc.SizeInBytes});
	ReleaseSRWLockExclusive(&geometry_heap_lock);
}

Dx12GeometryPlacement Dx12GeometryRegisterPlacement(ID3D12Device *device, ID3D12Heap *heap,
		UINT64 offset, const D3D12_RESOURCE_DESC *legacy_desc)
{
	Dx12GeometryPlacement placement = {};
	placement.placed = true;
	Microsoft::WRL::ComPtr<IUnknown> key;
	if (!heap || FAILED(heap->QueryInterface(IID_PPV_ARGS(&key))))
		return placement;

	AcquireSRWLockShared(&geometry_heap_lock);
	auto found = geometry_heaps.find(key.Get());
	const bool tracked = found != geometry_heaps.end() && found->second.device == device && !found->second.excluded;
	ReleaseSRWLockShared(&geometry_heap_lock);
	if (!tracked)
		return placement;

	D3D12_RESOURCE_ALLOCATION_INFO allocation = {};
	if (legacy_desc)
		allocation = device->GetResourceAllocationInfo(0, 1, legacy_desc);

	AcquireSRWLockExclusive(&geometry_heap_lock);
	found = geometry_heaps.find(key.Get());
	if (found != geometry_heaps.end() && found->second.device == device && !found->second.excluded) {
		auto &history = found->second;
		if (!allocation.Alignment || !allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
				offset % allocation.Alignment || offset > history.size || allocation.SizeInBytes > history.size - offset ||
				history.spans.size() >= geometry_spans_per_heap) {
			exclude_geometry_heap(history);
		} else {
			UINT64 begin = offset, end = offset + allocation.SizeInBytes;
			auto it = history.spans.lower_bound(begin);
			if (it != history.spans.begin() && std::prev(it)->second.end > begin)
				--it;
			bool overlap = false;
			while (it != history.spans.end() && it->first < end) {
				overlap = true;
				begin = min(begin, it->first);
				end = max(end, it->second.end);
				it = history.spans.erase(it);
			}
			const UINT64 serial = overlap ? 0 : ++geometry_placement_serial;
			history.spans.emplace(begin, GeometryHeapSpan{end, serial});
			placement = {key.Get(), history.serial, offset, allocation.SizeInBytes, serial, true};
		}
	}
	ReleaseSRWLockExclusive(&geometry_heap_lock);
	return placement;
}

void Dx12GeometryExcludeHeap(ID3D12Heap *heap)
{
	Microsoft::WRL::ComPtr<IUnknown> key;
	if (!heap || FAILED(heap->QueryInterface(IID_PPV_ARGS(&key))))
		return;
	AcquireSRWLockExclusive(&geometry_heap_lock);
	auto i = geometry_heaps.find(key.Get());
	if (i != geometry_heaps.end())
		exclude_geometry_heap(i->second);
	ReleaseSRWLockExclusive(&geometry_heap_lock);
}

const char *Dx12GeometryPlacementStatus(const Dx12GeometryPlacement &placement)
{
	const char *status = "placed_heap_unproven";
	if (!placement.heap)
		return status;
	AcquireSRWLockShared(&geometry_heap_lock);
	auto i = geometry_heaps.find(placement.heap);
	if (i != geometry_heaps.end() && i->second.serial == placement.heap_serial && !i->second.excluded) {
		const auto &spans = i->second.spans;
		auto span = spans.find(placement.offset);
		status = "placed_alias_overlap";
		if (placement.allocation_serial && span != spans.end() && span->second.serial == placement.allocation_serial)
			status = nullptr;
	}
	ReleaseSRWLockShared(&geometry_heap_lock);
	return status;
}

static volatile LONG64 resource_serial;

// See Dx12TextureStamp. Counted up with the lock of the resource's shard held.
static volatile LONG64 texture_stamps[4096];

static volatile LONG64* texture_stamp(ID3D12Resource *resource)
{
	return &texture_stamps[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 52];
}

UINT64 Dx12TextureStamp(ID3D12Resource *resource)
{
	return (UINT64)*texture_stamp(resource);
}

static ResourceShard* shard_of(ID3D12Resource *resource)
{
	return &shards[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 58];
}

static void resource_destroyed(void *object)
{
	ID3D12Resource *resource = (ID3D12Resource*)object;
	ResourceShard *shard = shard_of(resource);
	UINT64 va = 0;

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		va = i->second.va;
		shard->resources.erase(i);
	}
	InterlockedIncrement64(texture_stamp(resource));
	ReleaseSRWLockExclusive(&shard->lock);

	if (va) {
		AcquireSRWLockExclusive(&buffer_lock);
		auto range = buffers.find(va);
		if (range != buffers.end() && range->second.resource == resource) {
			stamp_buffer(va, range->second.end);
			buffers.erase(range);
		}
		ReleaseSRWLockExclusive(&buffer_lock);

		Dx12ForgetBufferHashes(resource);
	}
}

void Dx12TrackResource(REFIID riid, void **ppvResource, LONG initial_state, bool geometry_safe_committed, Dx12GeometryResourceOrigin geometry_origin)
{
	ID3D12Resource *resource = NULL;
	D3D12_HEAP_PROPERTIES heap = {};
	Dx12ResourceInfo info = {};
	ResourceShard *shard;

	if (!ppvResource || !*ppvResource)
		return;
	if (FAILED(((IUnknown*)*ppvResource)->QueryInterface(IID_PPV_ARGS(&resource))))
		return;

	info.desc = resource->GetDesc();
	info.state = initial_state;
	info.geometry_safe_committed = geometry_safe_committed;
	// Capture can keep a resource alive until its staging fence, but a placed
	// resource does not own its backing storage. Attach the heap to the
	// eligible resource, without keeping the resource alive in our tracker.
	if (geometry_origin.placement.allocation_serial &&
			info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
			info.desc.Flags == D3D12_RESOURCE_FLAG_NONE &&
			FAILED(resource->SetPrivateDataInterface(geometry_backing_heap_pin, geometry_origin.placement.heap)))
		geometry_origin.placement.heap = nullptr;
	info.geometry_origin = geometry_origin;
	info.top_state = initial_state;
	info.serial = InterlockedIncrement64(&resource_serial);
	// Fails for reserved resources, which have no heap of their own
	info.geometry_heap_query = resource->GetHeapProperties(&heap, &info.geometry_heap_flags);
	if (SUCCEEDED(info.geometry_heap_query)) {
		info.geometry_heap_properties = heap;
		info.heap_type = heap.Type;
		info.cpu_accessible = heap.Type == D3D12_HEAP_TYPE_UPLOAD
			|| heap.Type == D3D12_HEAP_TYPE_READBACK
			|| heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD
			|| (heap.Type == D3D12_HEAP_TYPE_CUSTOM && heap.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
	}
	if (info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		info.va = resource->GetGPUVirtualAddress();

	shard = shard_of(resource);
	AcquireSRWLockExclusive(&shard->lock);
	shard->resources[resource] = info;
	InterlockedIncrement64(texture_stamp(resource));
	ReleaseSRWLockExclusive(&shard->lock);

	if (info.va) {
		AcquireSRWLockExclusive(&buffer_lock);
		// One that was at the address before and is still in the map
		// may have reached further
		auto previous = buffers.find(info.va);
		if (previous != buffers.end())
			stamp_buffer(info.va, previous->second.end);
		buffers[info.va] = { info.va + info.desc.Width, resource };
		stamp_buffer(info.va, info.va + info.desc.Width);
		ReleaseSRWLockExclusive(&buffer_lock);
	}

	Dx12NotifyDestruction(resource, resource_destroyed);

	resource->Release();
}

// A legacy split transition may still be pending and the texture tracker
// deliberately records only its ending half. Keep this capture stricter,
// without changing the state used by the existing texture and Mod paths.
void Dx12ExcludeGeometryBarrierResource(ID3D12Resource *resource)
{
	if (!resource)
		return;
	ResourceShard *shard = shard_of(resource);
	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end() && i->second.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		i->second.geometry_barrier_unproven = true;
	ReleaseSRWLockExclusive(&shard->lock);
}

bool Dx12GetResourceInfo(ID3D12Resource *resource, Dx12ResourceInfo *info)
{
	ResourceShard *shard = shard_of(resource);
	bool found = false;

	if (!resource)
		return false;

	AcquireSRWLockShared(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		*info = i->second;
		found = true;
	}
	ReleaseSRWLockShared(&shard->lock);

	return found;
}

// The resources with Evict calls held back, each with a reference of ours so
// that Dx12TakeExpiredEvictions can pass those on whatever the game does with
// it. Taken after the lock of the resource's shard.
static SRWLOCK held_lock = SRWLOCK_INIT;
static std::unordered_set<ID3D12Resource*> held_resources;

// With the shard lock held, for the first Evict held back
static void hold(ID3D12Resource *resource)
{
	resource->AddRef();
	AcquireSRWLockExclusive(&held_lock);
	held_resources.insert(resource);
	ReleaseSRWLockExclusive(&held_lock);
}

// With the shard lock held, once none is held back any longer. The caller
// releases the reference after the shard lock.
static void unhold(ID3D12Resource *resource)
{
	AcquireSRWLockExclusive(&held_lock);
	held_resources.erase(resource);
	ReleaseSRWLockExclusive(&held_lock);
}

bool Dx12GameEvict(ID3D12Pageable *object, ULONGLONG read_since, bool (*reached)(UINT64 fence))
{
	// Only a key: a heap or another pageable is simply not found
	ID3D12Resource *resource = static_cast<ID3D12Resource*>(object);
	ResourceShard *shard = shard_of(resource);
	bool held = false;

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		Dx12ResourceInfo &info = i->second;

		info.game_evicted = true;
		// Once one is held back, the rest wait for
		// Dx12TakeExpiredEvictions as well
		if (info.evictions_held || (info.read_time && (info.read_time >= read_since || !reached(info.read_fence)))) {
			if (!info.evictions_held++)
				hold(resource);
			held = true;
		}
	}
	ReleaseSRWLockExclusive(&shard->lock);
	return held;
}

void Dx12GameMadeResident(ID3D12Pageable *object, bool succeeded)
{
	ID3D12Resource *resource = static_cast<ID3D12Resource*>(object);
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end())
		i->second.game_evicted = !succeeded;
	ReleaseSRWLockExclusive(&shard->lock);
}

Dx12GameReadResult Dx12GameRead(ID3D12Resource *resource, ULONGLONG now, UINT64 fence, HRESULT (*make_resident)(ID3D12Pageable *object))
{
	ResourceShard *shard = shard_of(resource);
	Dx12GameReadResult result = DX12_GAME_READ_RESIDENT;

	// Held while making it resident, so that the game's own calls for it
	// come after and see whether that worked
	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		if (i->second.game_evicted && !i->second.evictions_held) {
			result = DX12_GAME_READ_FAILED;
			if (SUCCEEDED(make_resident(resource))) {
				i->second.evictions_held = 1;
				hold(resource);
				result = DX12_GAME_READ_RESTORED;
			}
		}
		// A read left out leaves what an earlier one did
		if (result != DX12_GAME_READ_FAILED) {
			i->second.read_time = now;
			i->second.read_fence = fence;
		}
	}
	ReleaseSRWLockExclusive(&shard->lock);
	return result;
}

void Dx12TakeExpiredEvictions(ULONGLONG read_before, bool (*reached)(UINT64 fence), std::vector<Dx12HeldEviction> *expired)
{
	std::vector<ID3D12Resource*> candidates;

	AcquireSRWLockShared(&held_lock);
	candidates.assign(held_resources.begin(), held_resources.end());
	ReleaseSRWLockShared(&held_lock);

	// Alive for the reference that is held with them
	for (ID3D12Resource *resource : candidates) {
		ResourceShard *shard = shard_of(resource);

		AcquireSRWLockExclusive(&shard->lock);
		auto i = shard->resources.find(resource);
		if (i != shard->resources.end() && i->second.evictions_held
		    && i->second.read_time < read_before && reached(i->second.read_fence)) {
			expired->push_back({ resource, i->second.evictions_held });
			i->second.evictions_held = 0;
			unhold(resource);
		}
		ReleaseSRWLockExclusive(&shard->lock);
	}
}

void Dx12ApplyTransition(ID3D12Resource *resource, UINT subresource, LONG state)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockShared(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		const D3D12_RESOURCE_DESC *desc = &i->second.desc;

		if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || !subresource)
			InterlockedExchange(&i->second.top_state, state);

		// A single state is all that is kept per resource, so a
		// transition of one subresource out of several leaves it
		// unknown. Depth stencil formats have a subresource per plane.
		if (subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
		    && desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
		    && (desc->MipLevels != 1
		     || (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc->DepthOrArraySize != 1)
		     || (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
			state = DX12_STATE_UNKNOWN;

		InterlockedExchange(&i->second.state, state);
	}
	ReleaseSRWLockShared(&shard->lock);
}

void Dx12SetTextureDataHash(ID3D12Resource *resource, bool has_data_hash, uint32_t data_hash)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end() && (i->second.has_data_hash != has_data_hash || i->second.data_hash != data_hash)) {
		i->second.has_data_hash = has_data_hash;
		i->second.data_hash = data_hash;
		InterlockedIncrement64(texture_stamp(resource));
	}
	ReleaseSRWLockExclusive(&shard->lock);
}

bool Dx12NoteTopMipUpload(ID3D12Resource *resource)
{
	ResourceShard *shard = shard_of(resource);
	UINT frame = dx12_frame;
	bool hash_it = true;

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		Dx12ResourceInfo &info = i->second;

		if (info.top_uploaded && frame == info.top_upload_frame) {
			// Counted already
		} else if (info.top_uploaded && frame - info.top_upload_frame < DX12_HOT_FRAMES) {
			if (info.top_upload_hot < DX12_HOT_TEXTURE_LIMIT)
				info.top_upload_hot++;
		} else {
			info.top_upload_hot = 0;
		}
		info.top_uploaded = true;
		info.top_upload_frame = frame;
		hash_it = info.top_upload_hot < DX12_HOT_TEXTURE_LIMIT;
	}
	ReleaseSRWLockExclusive(&shard->lock);

	return hash_it;
}

void Dx12MarkCubeTexture(ID3D12Resource *resource)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end() && !i->second.cube) {
		i->second.cube = true;
		InterlockedIncrement64(texture_stamp(resource));
	}
	ReleaseSRWLockExclusive(&shard->lock);
}

// The buffers found before, by the 64 KiB of address space asked about, for
// the threads recording: they ask for the buffers of every draw that are not
// the same from frame to frame, with the map behind a lock they all share.
// What a slot has goes while the stamp of where it was asked is the one taken
// before asking. seq is odd while a slot is being written.
struct FoundBuffer {
	volatile LONG seq;
	UINT64 granule, start, end, stamp;
	ID3D12Resource *resource;
};
static FoundBuffer found_buffers[4096];

bool Dx12FindBuffer(UINT64 va, ID3D12Resource **resource, UINT64 *offset)
{
	FoundBuffer *slot, remembered;
	UINT64 stamp;
	bool found = false;
	LONG seq;

	if (!va)
		return false;

	stamp = Dx12FindBufferStamp(va);
	slot = &found_buffers[((va >> VA_STAMP_SHIFT) * 0x9e3779b97f4a7c15ull) >> 52];
	seq = slot->seq;
	if (!(seq & 1)) {
		remembered.granule = slot->granule;
		remembered.start = slot->start;
		remembered.end = slot->end;
		remembered.stamp = slot->stamp;
		remembered.resource = slot->resource;
		if (slot->seq == seq && remembered.granule == va >> VA_STAMP_SHIFT && remembered.stamp == stamp
		    && va >= remembered.start && va < remembered.end) {
			*resource = remembered.resource;
			*offset = va - remembered.start;
			return true;
		}
	}

	AcquireSRWLockShared(&buffer_lock);
	auto i = buffers.upper_bound(va);
	if (i != buffers.begin()) {
		--i;
		if (va < i->second.end) {
			*resource = i->second.resource;
			*offset = va - i->first;
			remembered.start = i->first;
			remembered.end = i->second.end;
			found = true;
		}
	}
	ReleaseSRWLockShared(&buffer_lock);

	if (found && !(seq & 1) && InterlockedCompareExchange(&slot->seq, seq + 1, seq) == seq) {
		slot->granule = va >> VA_STAMP_SHIFT;
		slot->start = remembered.start;
		slot->end = remembered.end;
		slot->stamp = stamp;
		slot->resource = *resource;
		InterlockedExchange(&slot->seq, seq + 2);
	}

	return found;
}
