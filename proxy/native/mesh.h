// Static meshes natively (mesh.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

namespace rnative::mesh {

void Install(HMODULE orig);

// Called with each RTriMesh_t read from an archive (its data loaded): the rvk backend's test path for mesh identity
// (RANDYVK_STATIC_ID, rvk_materials.cpp) - the game's meshes are identified through the exported RTriMesh_t::Archive.
void SetReadHook(void (*hook)(void* triMesh));

}  // namespace rnative::mesh
