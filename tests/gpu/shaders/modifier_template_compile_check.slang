// Build-time compile check for the engine's checked-in modifier example.
//
// engine/core/shaders/modifier_template.slang is documentation as much as code:
// apps copy it. It is not a row in the engine shader target (it ships no .spv),
// so nothing else would catch it drifting from modifier_common.slang or from
// the Vertex layout. Compiling it here keeps the example honest at no runtime
// cost; the generated SPIR-V is never loaded.
#include "../../../engine/core/shaders/modifier_template.slang"
