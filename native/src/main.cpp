#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <superblt_flat.h>

namespace {
constexpr int kPresentSlot = 8;
constexpr int kDrawIndexedSlot = 12;
constexpr int kDrawSlot = 13;
constexpr int kMapSlot = 14;
constexpr int kUnmapSlot = 15;
constexpr int kDrawIndexedInstancedSlot = 20;
constexpr int kDrawInstancedSlot = 21;
constexpr int kSetRenderTargetsSlot = 33;

constexpr int kShadowTableSize = 256;
constexpr int kThunkSize = 16;

constexpr float kMinScale = 1.0f;
constexpr float kMaxScale = 4.0f;
constexpr float kScaleEpsilon = 0.001f;

constexpr float kAttachedDistance = 2.0f;

constexpr float kRowLengthTolerance = 0.003f;
constexpr float kAspectTolerance = 0.002f;
constexpr float kMinProjectionRow = 0.2f;
constexpr int kRowFloats = 4;

constexpr uint32_t kSnapshotBytes = 768;
constexpr uint32_t kMaxSnapshotBuffer = 1024;

constexpr uint32_t kViewRowsBeforeProjection = 8;
constexpr uint32_t kMaxRowsBetweenMatrices = 2;

constexpr int kMaxViewmodelParts = 64;

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
using SetRenderTargetsFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
using MapFn = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
using UnmapFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);

PresentFn original_present = nullptr;

enum class Pass {
	None,
	Depth,
	Color
};

std::atomic<float> viewmodel_scale{1.0f};
std::atomic<bool> context_hooked{false};

ID3D11Device *game_device = nullptr;
ID3D11DeviceContext *game_context = nullptr;
void **runtime_table = nullptr;
void *shadow_table[kShadowTableSize] = {};

Pass current_pass = Pass::None;
ID3D11RenderTargetView *bound_target = nullptr;
ID3D11DepthStencilView *bound_depth = nullptr;
int depth_pass_draws = 0;
int color_pass_draws = 0;
bool drawing_lights = false;

struct Snapshot {
	uint32_t size = 0;
	float data[kSnapshotBytes / sizeof(float)] = {};
};

struct Origin {
	float x, y, z;
};

struct Mesh {
	ID3D11Buffer *index_buffer;
	UINT count;
	UINT start;
};

std::unordered_map<ID3D11Resource *, Snapshot> snapshots;
ID3D11Resource *mapped_resource = nullptr;
const void *mapped_data = nullptr;

float projection_x = 0.0f;
float projection_y = 0.0f;
Origin viewmodel_origins[kMaxViewmodelParts];
int viewmodel_origin_count = 0;
Mesh viewmodel_meshes[kMaxViewmodelParts];
int viewmodel_mesh_count = 0;

int frame_depth_draws = 0;

float frame_scale = 1.0f;
bool frame_scale_latched = false;

float latched_scale()
{
	if (!frame_scale_latched) {
		frame_scale = viewmodel_scale.load();
		frame_scale_latched = true;
	}

	return frame_scale;
}

void **vtable_of(void *object)
{
	return *reinterpret_cast<void ***>(object);
}

template <typename Fn>
Fn runtime_function(int slot)
{
	return reinterpret_cast<Fn>(runtime_table[slot]);
}

DXGI_FORMAT target_format(ID3D11RenderTargetView *target)
{
	if (!target) {
		return DXGI_FORMAT_UNKNOWN;
	}

	ID3D11Resource *resource = nullptr;
	target->GetResource(&resource);

	if (!resource) {
		return DXGI_FORMAT_UNKNOWN;
	}

	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *texture = nullptr;

	if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture)))) {
		D3D11_TEXTURE2D_DESC desc = {};
		texture->GetDesc(&desc);

		format = desc.Format;

		texture->Release();
	}

	resource->Release();

	return format;
}

void STDMETHODCALLTYPE hook_set_render_targets(ID3D11DeviceContext *context, UINT count, ID3D11RenderTargetView *const *targets, ID3D11DepthStencilView *depth)
{
	ID3D11RenderTargetView *target = count > 0 && targets ? targets[0] : nullptr;

	if (context == game_context && (target != bound_target || depth != bound_depth)) {
		bound_target = target;
		bound_depth = depth;

		DXGI_FORMAT format = target_format(target);

		drawing_lights = format == DXGI_FORMAT_R16G16B16A16_FLOAT;

		if (format == DXGI_FORMAT_R32_FLOAT && depth) {
			current_pass = Pass::Depth;
			latched_scale();
			depth_pass_draws = 0;
		} else if (current_pass == Pass::Depth && depth_pass_draws > 0) {
			current_pass = Pass::Color;
			color_pass_draws = 0;
		} else {
			current_pass = Pass::None;
		}
	}

	runtime_function<SetRenderTargetsFn>(kSetRenderTargetsSlot)(context, count, targets, depth);
}

float row_length(const float *row)
{
	return sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
}

const Snapshot *bound_snapshot()
{
	ID3D11Buffer *buffer = nullptr;
	game_context->VSGetConstantBuffers(0, 1, &buffer);

	if (!buffer) {
		return nullptr;
	}

	auto found = snapshots.find(buffer);

	buffer->Release();

	return found == snapshots.end() ? nullptr : &found->second;
}

void learn_projection(const Snapshot &snapshot)
{
	D3D11_VIEWPORT viewport = {};
	UINT count = 1;
	game_context->RSGetViewports(&count, &viewport);

	if (count == 0 || viewport.Height <= 0.0f) {
		return;
	}

	float aspect = viewport.Width / viewport.Height;
	uint32_t rows = snapshot.size / (kRowFloats * sizeof(float));

	for (uint32_t row = 0; row + 1 < rows; row++) {
		float x = row_length(&snapshot.data[row * kRowFloats]);
		float y = row_length(&snapshot.data[(row + 1) * kRowFloats]);

		if (x > kMinProjectionRow && fabsf(y / x - aspect) < kAspectTolerance) {
			projection_x = x;
			projection_y = y;

			return;
		}
	}
}

bool is_last_matrix_row(const float *row)
{
	return row[0] == 0.0f && row[1] == 0.0f && row[2] == 0.0f && row[3] == 1.0f;
}

bool origin_from_separate_matrices(const Snapshot &snapshot, uint32_t projection_row, Origin &origin)
{
	if (projection_row < kViewRowsBeforeProjection) {
		return false;
	}

	uint32_t view_row = projection_row - kViewRowsBeforeProjection;
	const float *view = &snapshot.data[view_row * kRowFloats];

	if (!is_last_matrix_row(view + kRowFloats * 3)) {
		return false;
	}

	for (uint32_t gap = 1; gap <= kMaxRowsBetweenMatrices && gap + 3 <= view_row; gap++) {
		const float *world_last = &snapshot.data[(view_row - gap) * kRowFloats];

		if (!is_last_matrix_row(world_last)) {
			continue;
		}

		const float *world = world_last - kRowFloats * 3;
		float x = world[3];
		float y = world[kRowFloats + 3];
		float z = world[kRowFloats * 2 + 3];
		float view_space[3];

		for (int i = 0; i < 3; i++) {
			const float *row = view + i * kRowFloats;

			view_space[i] = row[0] * x + row[1] * y + row[2] * z + row[3];
		}

		origin = {view_space[0], view_space[1], view_space[2]};

		return true;
	}

	return false;
}

bool find_origin(const Snapshot &snapshot, Origin &origin)
{
	if (projection_x == 0.0f) {
		return false;
	}

	uint32_t rows = snapshot.size / (kRowFloats * sizeof(float));

	for (uint32_t row = 0; row + 3 < rows; row++) {
		const float *x_row = &snapshot.data[row * kRowFloats];
		const float *y_row = x_row + kRowFloats;
		const float *w_row = y_row + kRowFloats * 2;

		if (fabsf(row_length(x_row) - projection_x) < kRowLengthTolerance && fabsf(row_length(y_row) - projection_y) < kRowLengthTolerance) {
			if (fabsf(w_row[3]) < 0.01f) {
				return origin_from_separate_matrices(snapshot, row, origin);
			}

			origin.x = x_row[3] / projection_x;
			origin.y = y_row[3] / projection_y;
			origin.z = w_row[3];

			return true;
		}
	}

	return false;
}

void note_viewmodel_part(UINT count, UINT start)
{
	ID3D11Buffer *index_buffer = nullptr;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT offset = 0;
	game_context->IAGetIndexBuffer(&index_buffer, &format, &offset);

	if (index_buffer) {
		if (viewmodel_mesh_count < kMaxViewmodelParts) {
			viewmodel_meshes[viewmodel_mesh_count++] = {index_buffer, count, start};
		}

		index_buffer->Release();
	}

	const Snapshot *snapshot = bound_snapshot();

	if (!snapshot) {
		return;
	}

	if (projection_x == 0.0f) {
		learn_projection(*snapshot);
	}

	Origin origin;

	if (viewmodel_origin_count < kMaxViewmodelParts && find_origin(*snapshot, origin)) {
		viewmodel_origins[viewmodel_origin_count++] = origin;
	}
}

bool reuses_viewmodel_mesh(UINT count, UINT start)
{
	ID3D11Buffer *index_buffer = nullptr;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	UINT offset = 0;
	game_context->IAGetIndexBuffer(&index_buffer, &format, &offset);

	if (!index_buffer) {
		return false;
	}

	index_buffer->Release();

	for (int i = 0; i < viewmodel_mesh_count; i++) {
		const Mesh &mesh = viewmodel_meshes[i];

		if (mesh.index_buffer == index_buffer && mesh.count == count && mesh.start == start) {
			return true;
		}
	}

	return false;
}

bool sits_on_viewmodel()
{
	if (drawing_lights) {
		return false;
	}

	const Snapshot *snapshot = bound_snapshot();
	Origin origin;

	if (!snapshot || !find_origin(*snapshot, origin)) {
		return false;
	}

	for (int i = 0; i < viewmodel_origin_count; i++) {
		float x = origin.x - viewmodel_origins[i].x;
		float y = origin.y - viewmodel_origins[i].y;
		float z = origin.z - viewmodel_origins[i].z;

		if (x * x + y * y + z * z < kAttachedDistance * kAttachedDistance) {
			return true;
		}
	}

	return false;
}

bool next_draw_is_viewmodel(UINT count, UINT start)
{
	if (current_pass == Pass::Depth) {
		depth_pass_draws++;
		frame_depth_draws++;

		note_viewmodel_part(count, start);

		return true;
	}

	if (current_pass == Pass::Color && color_pass_draws < depth_pass_draws) {
		color_pass_draws++;

		return true;
	}

	if (frame_depth_draws == 0) {
		return false;
	}

	return reuses_viewmodel_mesh(count, start) || sits_on_viewmodel();
}

class ScaledViewports {
public:
	ScaledViewports(ID3D11DeviceContext *context, UINT draw_count, UINT draw_start) : context_(context)
	{
		if (context != game_context || !next_draw_is_viewmodel(draw_count, draw_start)) {
			return;
		}

		float scale = latched_scale();

		if (scale < kMinScale + kScaleEpsilon) {
			return;
		}

		count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&count_, saved_);

		if (count_ == 0) {
			return;
		}

		D3D11_VIEWPORT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

		for (UINT i = 0; i < count_; i++) {
			scaled[i] = saved_[i];
			scaled[i].Width = saved_[i].Width * scale;
			scaled[i].Height = saved_[i].Height * scale;
			scaled[i].TopLeftX = saved_[i].TopLeftX + (saved_[i].Width - scaled[i].Width) * 0.5f;
			scaled[i].TopLeftY = saved_[i].TopLeftY + (saved_[i].Height - scaled[i].Height) * 0.5f;
		}

		context->RSSetViewports(count_, scaled);
	}

	~ScaledViewports()
	{
		if (count_ > 0) {
			context_->RSSetViewports(count_, saved_);
		}
	}

private:
	ID3D11DeviceContext *context_;
	UINT count_ = 0;
	D3D11_VIEWPORT saved_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
};

void STDMETHODCALLTYPE hook_draw_indexed(ID3D11DeviceContext *context, UINT count, UINT start, INT base_vertex)
{
	ScaledViewports viewports(context, count, start);

	runtime_function<DrawIndexedFn>(kDrawIndexedSlot)(context, count, start, base_vertex);
}

void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext *context, UINT count, UINT start)
{
	ScaledViewports viewports(context, count, start);

	runtime_function<DrawFn>(kDrawSlot)(context, count, start);
}

void STDMETHODCALLTYPE hook_draw_indexed_instanced(ID3D11DeviceContext *context, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	ScaledViewports viewports(context, count, start);

	runtime_function<DrawIndexedInstancedFn>(kDrawIndexedInstancedSlot)(context, count, instances, start, base_vertex, start_instance);
}

void STDMETHODCALLTYPE hook_draw_instanced(ID3D11DeviceContext *context, UINT count, UINT instances, UINT start, UINT start_instance)
{
	ScaledViewports viewports(context, count, start);

	runtime_function<DrawInstancedFn>(kDrawInstancedSlot)(context, count, instances, start, start_instance);
}

bool copy_mapped(void *destination, const void *source, size_t size)
{
	__try {
		memcpy(destination, source, size);

		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void snapshot_constant_buffer(ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
	resource->GetType(&dimension);

	if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
		return;
	}

	D3D11_BUFFER_DESC desc = {};
	static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);

	if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || desc.ByteWidth > kMaxSnapshotBuffer) {
		return;
	}

	Snapshot &snapshot = snapshots[resource];
	uint32_t size = desc.ByteWidth < kSnapshotBytes ? desc.ByteWidth : kSnapshotBytes;

	snapshot.size = copy_mapped(snapshot.data, mapped_data, size) ? size : 0;
}

HRESULT STDMETHODCALLTYPE hook_map(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
	HRESULT result = runtime_function<MapFn>(kMapSlot)(context, resource, subresource, type, flags, mapped);

	if (SUCCEEDED(result) && mapped && type == D3D11_MAP_WRITE_DISCARD) {
		mapped_resource = resource;
		mapped_data = mapped->pData;
	}

	return result;
}

void STDMETHODCALLTYPE hook_unmap(ID3D11DeviceContext *context, ID3D11Resource *resource, UINT subresource)
{
	bool wanted = current_pass != Pass::None || frame_depth_draws > 0;

	if (wanted && resource == mapped_resource && viewmodel_scale.load() >= kMinScale + kScaleEpsilon) {
		snapshot_constant_buffer(resource);
	}

	mapped_resource = nullptr;

	runtime_function<UnmapFn>(kUnmapSlot)(context, resource, subresource);
}

bool install_shadow_table()
{
	runtime_table = vtable_of(game_context);

	uint8_t *thunks = static_cast<uint8_t *>(VirtualAlloc(nullptr, kShadowTableSize * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

	if (!thunks) {
		return false;
	}

	for (int slot = 0; slot < kShadowTableSize; slot++) {
		uint8_t *thunk = thunks + slot * kThunkSize;
		uint64_t table = reinterpret_cast<uint64_t>(runtime_table);
		uint32_t offset = slot * sizeof(void *);

		thunk[0] = 0x48;
		thunk[1] = 0xB8;
		memcpy(thunk + 2, &table, sizeof(table));

		thunk[10] = 0xFF;
		thunk[11] = 0xA0;
		memcpy(thunk + 12, &offset, sizeof(offset));

		shadow_table[slot] = thunk;
	}

	DWORD old_protect = 0;
	VirtualProtect(thunks, kShadowTableSize * kThunkSize, PAGE_EXECUTE_READ, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), thunks, kShadowTableSize * kThunkSize);

	shadow_table[kDrawIndexedSlot] = reinterpret_cast<void *>(hook_draw_indexed);
	shadow_table[kDrawSlot] = reinterpret_cast<void *>(hook_draw);
	shadow_table[kMapSlot] = reinterpret_cast<void *>(hook_map);
	shadow_table[kUnmapSlot] = reinterpret_cast<void *>(hook_unmap);
	shadow_table[kDrawIndexedInstancedSlot] = reinterpret_cast<void *>(hook_draw_indexed_instanced);
	shadow_table[kDrawInstancedSlot] = reinterpret_cast<void *>(hook_draw_instanced);
	shadow_table[kSetRenderTargetsSlot] = reinterpret_cast<void *>(hook_set_render_targets);

	*reinterpret_cast<void ***>(game_context) = shadow_table;

	return true;
}

void hook_game_context(IDXGISwapChain *swap_chain)
{
	context_hooked = true;

	if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&game_device)))) {
		return;
	}

	game_device->GetImmediateContext(&game_context);

	install_shadow_table();
}

void end_frame()
{
	if (game_context && vtable_of(game_context) != shadow_table) {
		*reinterpret_cast<void ***>(game_context) = shadow_table;
	}

	current_pass = Pass::None;
	bound_target = nullptr;
	bound_depth = nullptr;
	frame_depth_draws = 0;
	frame_scale_latched = false;
	viewmodel_origin_count = 0;
	viewmodel_mesh_count = 0;
	projection_x = 0.0f;
	projection_y = 0.0f;
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *swap_chain, UINT sync_interval, UINT flags)
{
	if (!context_hooked) {
		hook_game_context(swap_chain);
	}

	end_frame();

	return original_present(swap_chain, sync_interval, flags);
}

void patch_present(void **vtable)
{
	DWORD old_protect = 0;

	VirtualProtect(&vtable[kPresentSlot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect);

	original_present = reinterpret_cast<PresentFn>(vtable[kPresentSlot]);
	vtable[kPresentSlot] = reinterpret_cast<void *>(hook_present);

	VirtualProtect(&vtable[kPresentSlot], sizeof(void *), old_protect, &old_protect);
}

void install_hooks()
{
	WNDCLASSEXA window_class = {};
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = DefWindowProcA;
	window_class.hInstance = GetModuleHandleA(nullptr);
	window_class.lpszClassName = "VMFOVDummy";
	RegisterClassExA(&window_class);

	HWND window = CreateWindowExA(0, window_class.lpszClassName, "", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, window_class.hInstance, nullptr);

	DXGI_SWAP_CHAIN_DESC swap_desc = {};
	swap_desc.BufferCount = 1;
	swap_desc.BufferDesc.Width = 16;
	swap_desc.BufferDesc.Height = 16;
	swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_desc.OutputWindow = window;
	swap_desc.SampleDesc.Count = 1;
	swap_desc.Windowed = TRUE;
	swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	IDXGISwapChain *swap_chain = nullptr;
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;

	HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &swap_desc, &swap_chain, &device, nullptr, &context);

	if (FAILED(result)) {
		PD2HOOK_LOG_ERROR("Viewmodel FOV could not create a device to hook the renderer");
		DestroyWindow(window);
		return;
	}

	patch_present(vtable_of(swap_chain));

	context->Release();
	device->Release();
	swap_chain->Release();
	DestroyWindow(window);
}

int lua_set_scale(lua_State *L)
{
	float scale = static_cast<float>(luaL_checknumber(L, 1));

	if (scale < kMinScale) {
		scale = kMinScale;
	} else if (scale > kMaxScale) {
		scale = kMaxScale;
	}

	viewmodel_scale = scale;

	return 0;
}
}

void Plugin_Init()
{
	install_hooks();
}

void Plugin_Update()
{
}

void Plugin_Setup_Lua(lua_State *L)
{
}

int Plugin_PushLua(lua_State *L)
{
	lua_newtable(L);

	lua_pushcfunction(L, lua_set_scale);
	lua_setfield(L, -2, "set_scale");

	return 1;
}
