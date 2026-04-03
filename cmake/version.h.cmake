set(VERSION_HEADER_PATH "${CMAKE_SOURCE_DIR}/mod_http3/include/version.h")

set(VERSION_HEADER_CONTENT
"/* Auto-generated - do not edit */

#ifndef MOD_HTTP3_VERSION_H
#define MOD_HTTP3_VERSION_H

#define MOD_HTTP3_VERSION_MAJOR ${PROJECT_VERSION_MAJOR}
#define MOD_HTTP3_VERSION_MINOR ${PROJECT_VERSION_MINOR}
#define MOD_HTTP3_VERSION_PATCH ${PROJECT_VERSION_PATCH}

// Construct a 24-bit packed version number from major, minor and patch. Version 1.2.3 becomes 0x010203.
#define MOD_HTTP3_MAKE_VERSION(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))

#define MOD_HTTP3_VERSION MOD_HTTP3_MAKE_VERSION(MOD_HTTP3_VERSION_MAJOR, MOD_HTTP3_VERSION_MINOR, MOD_HTTP3_VERSION_PATCH)

#define MOD_HTTP3_VERSION_STRING \"${PROJECT_VERSION}\"

#endif
")

# Only rewrites when content changes (avoids unnecessary recompilation).
if(EXISTS "${VERSION_HEADER_PATH}")
  file(READ "${VERSION_HEADER_PATH}" CURRENT_VERSION_CONTENT)
else()
  set(CURRENT_VERSION_CONTENT "")
endif()

if(NOT CURRENT_VERSION_CONTENT STREQUAL VERSION_HEADER_CONTENT)
  file(WRITE "${VERSION_HEADER_PATH}" "${VERSION_HEADER_CONTENT}")
endif()
