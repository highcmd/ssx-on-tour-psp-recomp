#pragma once
struct CompatFlagsShim { bool MoreAccurateVMMUL = false; bool VprefixInvalid = false; };
struct CompatShim { CompatFlagsShim f; const CompatFlagsShim &flags() const { return f; } };
struct CoreParameterShim { CompatShim compat; };
CoreParameterShim &PSP_CoreParameter();
