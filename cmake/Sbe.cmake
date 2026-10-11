# SBE codec generation.
#
# Policy: generated headers are CHECKED IN under venues/<venue>/gen/. Ordinary
# builds therefore need no JVM and no network. Regeneration is opt-in:
#
#   cmake -S . -B build -DRFEED_REGENERATE_SBE=ON -DRFEED_SBE_JAR=/path/sbe-all-1.39.0.jar
#
# CI runs the regeneration and fails if the tree is dirty afterwards, so a
# schema change cannot land without its codecs.
#
# Generated code will not survive RFEED_WARNINGS (-Wconversion, -Wold-style-cast
# and friends), so it is always attached as a SYSTEM include directory. It is
# also excluded from .clang-tidy's HeaderFilterRegex.

option(RFEED_REGENERATE_SBE "Regenerate SBE codecs from schema XML (requires a JVM)" OFF)
set(RFEED_SBE_JAR "" CACHE FILEPATH "Path to sbe-all-<version>.jar")

# rfeed_add_venue(<name>)
#
# Expects venues/<name>/schema.xml. Creates an INTERFACE target rfeed_venue_<name>
# exposing venues/<name>/ and, when present, the generated codecs as SYSTEM.
function(rfeed_add_venue VENUE)
  set(VENUE_DIR "${CMAKE_SOURCE_DIR}/venues/${VENUE}")
  set(SCHEMA    "${VENUE_DIR}/schema.xml")
  set(GEN_DIR   "${VENUE_DIR}/gen")

  if(NOT EXISTS "${SCHEMA}")
    message(FATAL_ERROR "rfeed_add_venue(${VENUE}): missing ${SCHEMA}")
  endif()

  add_library(rfeed_venue_${VENUE} INTERFACE)
  target_include_directories(rfeed_venue_${VENUE} INTERFACE "${VENUE_DIR}")

  if(EXISTS "${GEN_DIR}")
    # SYSTEM: generated code is not ours to keep warning-clean.
    target_include_directories(rfeed_venue_${VENUE} SYSTEM INTERFACE "${GEN_DIR}")
  endif()

  if(RFEED_REGENERATE_SBE)
    find_package(Java 11 REQUIRED COMPONENTS Runtime)
    if(NOT RFEED_SBE_JAR OR NOT EXISTS "${RFEED_SBE_JAR}")
      message(FATAL_ERROR
        "RFEED_REGENERATE_SBE=ON but RFEED_SBE_JAR is unset or missing.\n"
        "Fetch it from https://github.com/aeron-io/simple-binary-encoding/releases")
    endif()

    add_custom_target(rfeed_gen_sbe_${VENUE}
      COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
      COMMAND ${Java_JAVA_EXECUTABLE}
              --add-opens java.base/jdk.internal.misc=ALL-UNNAMED
              -Dsbe.target.language=Cpp
              -Dsbe.target.namespace=rfeed_gen_${VENUE}
              -Dsbe.output.dir=${GEN_DIR}
              -Dsbe.validation.stop.on.error=true
              -Dsbe.validation.xsd.stop.on.error=true
              -jar "${RFEED_SBE_JAR}" "${SCHEMA}"
      COMMENT "Generating SBE codecs for venue '${VENUE}'"
      VERBATIM
    )
  endif()
endfunction()
