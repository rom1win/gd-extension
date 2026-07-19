#pragma once

#include <cstdio>

class TressFXAsset;

// Parses the Blender headless-export ".ghair" v1 format (tools/verify_ghair.py
// is the authoritative doc -- keep this in sync with it, not the other way
// around) directly into a TressFXAsset, filling exactly what
// TressFXAsset::LoadHairData fills for .tfx so the rest of the asset pipeline
// (GenerateFollowHairs/ProcessAsset) is unaware of which loader ran.
namespace GhairLoader {

// Fills asset->m_positions/m_strandUV/m_numGuideStrands/m_numVerticesPerStrand/
// m_numFollowStrandsPerGuide(=0)/m_numTotalStrands/m_numGuideVertices/
// m_numTotalVertices/m_followRootOffsets(zeroed). Strand count is padded up to
// a multiple of TRESSFX_SIM_THREAD_GROUP_SIZE by duplicating the last real
// strand's data -- the exact convention TressFXAsset::LoadHairData uses for
// .tfx (see TressFXAsset.cpp). Loud, non-fatal: push_warning + return false on
// any structural problem (bad magic, truncation, missing required chunk,
// invalid vps). `out_raw_num_strands`, if non-null, receives the strand count
// as it appeared in the file (pre-padding).
bool LoadHairData(FILE* file, const char* debug_path, TressFXAsset* asset, int* out_raw_num_strands = nullptr);

// Fills asset->m_boneSkinningData (size m_numTotalStrands, one entry per
// strand slot, guide AND follow -- unlike TressFXAsset::LoadBoneData's .tfx
// path, which only fills guide slots because untouched follow slots get
// overwritten by the UpdateFollowHairVertices kernel anyway; filling every
// slot here is simply more robust and costs nothing). Every strand rigidly
// bound 100% to bone 0 -- this loader has no per-strand bone weights (no
// authored skin data in .ghair v1), so it targets a single-bone test rig.
// Call after asset->GenerateFollowHairs() so m_numTotalStrands is final.
void FillUniformBoneSkinning(TressFXAsset* asset);

} // namespace GhairLoader
