# Live meshes and mesh atlases (plan, 2026-10-22)

ao-assets' workbench (its `docs/custom-textures-plan.md`) lets a static mesh be remodelled and given a texture of its
own. Two things in the renderer make that visible in the running game:

- **Mesh atlas**: maps named after the mesh, `1010001_<id>_d.png` / `_n` / `_orm` / `_e` / `_h` (from `live\`,
  `materials\` or a pack, as any texture's maps), drawn on every sub-mesh of that mesh instead of its material's
  texture.
- **Live geometry**: `1010001_<id>_mesh.rvkm` in `live\` (hot reloaded), `materials\` or a pack (an ao-assets authored
  pack's remodels: read at startup, found as the maps are), the sub-meshes drawn instead of the mesh's own, so a save
  in Blender shows up within a second, and a pack can carry new geometry, without rdb.db being written.

## Knowing which mesh a sub-mesh is

`MeshRead` (rvk_materials.cpp; the read hook of the native RTriMesh_t(archive) constructor, so `[Native] Scene` on)
learns each loaded static mesh's id from the RDBMesh_t being loaded on the stack. (The first version hooked the
exported `RTriMesh_t::Archive`, which is the const one that writes a mesh out and never runs in the game: only the
harness, naming its meshes with `RANDYVK_STATIC_ID`, ever saw a tag.) It also tags the mesh's
SimpleMeshes: tag = an entry {mesh key, piece name (its RTriMeshData_t's RResource name), sub-mesh index}. The tag lives
in randy_native (`meshdata::SetTag` / `Tag`): a SimpleMesh copy keeps it (MeshCopy - the clones the world draws share
the loaded data; lightmap copies clone the SimpleMeshes), its destructor drops it. No tags are kept while
`RVK_LiveMeshes` is off.

## The draw

`SimpleMesh::Render` (native mesh.cpp `MeshRender`; every static mesh pass goes through it: lit, depth pre-pass, shadow,
statel light, environment map) asks the direct channel when the sub-mesh has a tag and any override exists:

- `meshBegin(tag, vertexBuffer, vertexCount)` -> 0 draw as it is / 1 draw as it is, then `meshEnd` / 2 drawn instead,
  then `meshEnd`.
- The atlas: stage 0's texture is the atlas (an rvk texture with the decoded maps; the albedo drawn instead of its one
  pixel), `COLOROP` / `ALPHAOP` `MODULATE (TEXTURE, DIFFUSE)`, stage 1 disabled; `meshEnd` puts the device's own state
  back (RDevice's m_textures / m_tss). The game's DeviceState never sees a change.
- Live geometry: the replacement's vertices and indices with `DrawIndexedPrimitive`. A lightmapped mesh (statics in
  the playfields: 16-bit colours copied into its vertex buffer before each draw) gets each new vertex's colour from the
  nearest original vertex, read from the buffer as it is at the draw.

With no override files nothing changes: `calllog-ab Scene` stays identical (the tag lookup isn't made).

Known limits of live geometry: culling and the occluder still use the original's bounding volume (a much bigger
remodel can disappear at the screen's edge until it is applied); collision is the playfield's; picking (Ctrl+Shift+I/J)
tests the original's triangles. Applying the mesh (game closed) makes all of these the new mesh's.

## Files

`.rvkm` (little-endian; ao-assets livemesh.py writes it):
```
"RVKM" u32 version (1) u32 pieces
piece: u16 name length, name bytes (the RTriMeshData_t's name), u32 sub-mesh index (in the original),
       u32 fvf (0x112 or 0x152), u32 vertex count, u32 index count, vertices (fvf stride each), u16 indices
```
A sub-mesh of the original that has no piece draws nothing (its material's geometry moved to another sub-mesh).

## Testing

`randy_harness --static <archive>` reads its meshes without DisplaySystem, so there is no RDBMesh_t to identify them;
`RANDYVK_STATIC_ID=1010001:3545` makes them that mesh (a read hook on the native RTriMesh_t archive constructor,
installed only with the variable set). With `RANDYVK_HOTRELOAD=1` and files in the test client's `randy-vk\live\`
(ao-assets `livemesh.py` writes the geometry): the lamps of `3545.archive` drawn taller and with the atlas, the
lightmap's colours (`--lightmap`) carried to the new vertices, files copied in while the harness runs picked up.
`tools/calllog-ab.sh Scene statics lightmap staticshadow plain`: identical (no files: nothing looks a tag up).
