# Generate patchinfo.h from the putty-ultra.cfg configuration file, so
# that the patch version and author shown in the About box can be
# updated by editing that one text file.

set(PATCH_CFG_FILE ${CMAKE_SOURCE_DIR}/putty-ultra.cfg)
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY
  CMAKE_CONFIGURE_DEPENDS ${PATCH_CFG_FILE})

set(PATCH_VERSION "")
set(PATCH_AUTHOR "")

if(EXISTS ${PATCH_CFG_FILE})
  file(STRINGS ${PATCH_CFG_FILE} patch_cfg_lines)
  foreach(patch_cfg_line ${patch_cfg_lines})
    string(STRIP "${patch_cfg_line}" patch_cfg_line)
    if(patch_cfg_line STREQUAL "" OR patch_cfg_line MATCHES "^#")
      # blank line or comment
    elseif(patch_cfg_line MATCHES "^(PATCH_[A-Za-z0-9_]+)[ \t]*[=:][ \t]*(.*)$")
      set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
  endforeach()
else()
  message(WARNING "${PATCH_CFG_FILE} not found: the About box will not "
    "mention the patch version")
endif()

configure_file(${CMAKE_SOURCE_DIR}/cmake/patchinfo.h.in
  ${GENERATED_SOURCES_DIR}/patchinfo.h @ONLY)
