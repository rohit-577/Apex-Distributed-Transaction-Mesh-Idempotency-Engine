#pragma once

// Project-wide version constant. The single source of truth for the version
// string reported by /health is the APEX_VERSION compile definition set in
// CMakeLists.txt; this header only carries the phase marker so that code and
// documentation cannot disagree about which phase the binary belongs to.

#include <string_view>

namespace apex::core {

inline constexpr std::string_view kPhase = "phase-0";

}  // namespace apex::core
