// The device layer natively (device.cpp): randy-vk.ini [Native] Device=on. DeviceState (the render state cache),
// render_t's draw calls, transforms, lights and vertex buffer creation, VertexBuffer_c - in the original's layouts,
// talking to the same IDirect3DDevice7 / IDirect3DVertexBuffer7 (the call log of a frame is identical).
#pragma once

#include "native/native.h"

#include <cstdint>

namespace rnative::device {

void Install(HMODULE orig);

// VertexBuffer_c::GetFormatSize: bytes per vertex of an FVF, as Randy counts them (its table of FVF bits).
uint32_t FormatSize(uint32_t fvf);

// The direct channel to the renderer (docs/device-on-rvk.md): functions the rvk backend registers when it is the
// Direct3D 7 implementation. Each takes the IDirect3DDevice7 the call would have gone to and returns false when that
// isn't the backend's device - the caller then makes the D3D7 calls as before.
struct StateChange {
    enum Kind : uint32_t { RenderState, StageState, Texture } kind;
    uint32_t stage, type, value;            // RenderState: type = the state; Texture: stage
    void* surface;                          // Texture: the IDirectDrawSurface7 (or null)
};
struct Direct {
    // DeviceState::UpdateDevice's changes in the order it would make the calls (SetRenderState, SetTextureStageState,
    // SetTexture), as one.
    bool (*applyStates)(void* d3dDevice, const StateChange* changes, uint32_t count);
    // Game-thread time in a part of the native scene code (`name` a string literal), for the renderer's profile log.
    void (*gameSection)(const char* name, double ms);
    // IDirect3DDevice7::DrawIndexedPrimitiveVB whose indices belong to a native triangle list unchanged since
    // `indexGeneration` (meshdata::IndexGeneration): the backend may keep them instead of copying them each draw.
    bool (*drawIndexedVB)(void* d3dDevice, uint32_t type, void* d3dVertexBuffer, uint32_t start, uint32_t vertexCount,
                          const uint16_t* indices, uint32_t indexCount, uint64_t indexGeneration);
    // The renderer drops blob shadow draws (its sun shadows replace them).
    bool (*blobShadowsReplaced)();
    // Around the interface's drawing (gui.cpp, GUI.dll's WindowController_c::Render): begin returns whether to draw it
    // this frame - false: the renderer shows the last one drawn (its interface layer, RVK_UiRate). end after it.
    bool (*interfaceBegin)();
    void (*interfaceEnd)();
    // Around a tagged static sub-mesh's draw (meshdata::Tag; docs/live-meshes-plan.md), while SetMeshOverrides(true):
    // begin returns 0 to draw it as it is, 1 as it is and then call end (the backend changed its texture), 2 drawn
    // instead by the backend, then end. vertexBuffer: the IDirect3DVertexBuffer7 it is drawn from.
    uint32_t (*meshBegin)(void* d3dDevice, uint32_t tag, void* d3dVertexBuffer, uint32_t vertexCount);
    void (*meshEnd)(void* d3dDevice);
};
void SetDirect(const Direct* direct);       // null: none (the backend's device is gone)

// Whether any tagged sub-mesh has something drawn instead (the backend says so when its override files change): only
// then do native mesh draws look their tag up.
void SetMeshOverrides(bool any);
bool MeshOverrides();
// The direct channel's meshBegin / meshEnd for a draw through `render` (render_t) from `buffer` (a VertexBuffer_c):
// 0 without it.
uint32_t MeshBegin(void* render, uint32_t tag, void* buffer, uint32_t vertexCount);
void MeshEnd(void* render);

// True when the renderer draws its own shadows instead of the game's blob shadows (direct channel on): the blob
// shadows' per-frame work (cat_skin.cpp: its callback over skinned positions) can be skipped.
bool BlobShadowsReplaced();

// GUI boundaries use the backend callbacks even with Native Direct or Device off.
bool InterfaceBegin();
void InterfaceEnd();

// Around a native mesh's draw (mesh.cpp): its indices are a native triangle list's, unchanged while `generation`
// (meshdata::IndexGeneration, non-zero) is. [Native] Retain = on.
class RetainedIndices {
public:
    explicit RetainedIndices(uint64_t generation);
    ~RetainedIndices();
    RetainedIndices(const RetainedIndices&) = delete;
    RetainedIndices& operator=(const RetainedIndices&) = delete;

private:
    uint64_t m_previous;
};

// Times a part of the game thread's frame into the profile ("cpu ms" line) when the rvk backend is there. Nested
// timers (an offscreen viewport rendered inside a visual's render) count in the outermost only.
class GameTimer {
public:
    explicit GameTimer(const char* name);
    ~GameTimer();
    GameTimer(const GameTimer&) = delete;
    GameTimer& operator=(const GameTimer&) = delete;

private:
    const char* m_name;
    int64_t m_start = 0;                    // 0: not the outermost (or no backend)
};

// The same between two points that aren't one scope: GameClock() at the start (0 without the backend), then
// GameSectionSince(name, start) at the end.
int64_t GameClock();
void GameSectionSince(const char* name, int64_t start);

}  // namespace rnative::device
