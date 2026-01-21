#pragma once

// The build environment must define either ROLE_ROOT or ROLE_NODE
// For example, in platformio.ini:
// build_flags = -DROLE_ROOT  (for root)
// build_flags = -DROLE_NODE  (for node)

#if defined(ROLE_ROOT)
#define IS_ROOT 1
#elif defined(ROLE_NODE)
#define IS_ROOT 0
#endif
