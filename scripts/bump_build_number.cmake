# Increments buildNumber in a pdxinfo file in place.
# Usage: cmake -DPDXINFO=<path to pdxinfo> -P bump_build_number.cmake
# Run by the build (see CMakeLists.txt) just before pdc bundles pdxinfo, so
# every .pdx carries a build number one higher than the previous one.
if (NOT PDXINFO OR NOT EXISTS "${PDXINFO}")
	message(FATAL_ERROR "bump_build_number: PDXINFO not set or not found: '${PDXINFO}'")
endif()

file(READ "${PDXINFO}" content)

if (content MATCHES "(^|\n)buildNumber=([0-9]+)")
	math(EXPR next "${CMAKE_MATCH_2} + 1")
	string(REGEX REPLACE "(^|\n)buildNumber=[0-9]+" "\\1buildNumber=${next}" content "${content}")
else()
	# No buildNumber yet: start the count at 1 on its own line.
	set(next 1)
	if (NOT content STREQUAL "" AND NOT content MATCHES "\n$")
		string(APPEND content "\n")
	endif()
	string(APPEND content "buildNumber=${next}\n")
endif()

file(WRITE "${PDXINFO}" "${content}")
message(STATUS "pdxinfo buildNumber=${next}")
