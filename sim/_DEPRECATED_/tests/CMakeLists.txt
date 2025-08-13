# -------------------------------
# Executable Definitions
# -------------------------------
# List of Executables and Their Main Source Files.
add_executable(htsim_dumbell_uec main_dumbell_uec.cpp)

# -------------------------------
# Linking Executables with Libraries
# -------------------------------
set(ALL_EXECUTABLES
    htsim_dumbell_uec
)
foreach(EXECUTABLE ${ALL_EXECUTABLES})
    # Link each executable with the 'htsim' library defined in the parent directory.
    target_link_libraries(${EXECUTABLE} PRIVATE htsim)
    # Add a post-build command to explicitly remove and recreate the symlink for all previous executables.
    add_symlink_target(${EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
