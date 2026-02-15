function(hydra_add_ui_build target_name ui_dir)
  find_program(HYDRA_NPM_EXECUTABLE npm REQUIRED)

  add_custom_target(${target_name}
    COMMAND ${CMAKE_COMMAND} -E echo "[HydraStack] Building UI bundles"
    COMMAND ${HYDRA_NPM_EXECUTABLE} install
    COMMAND ${HYDRA_NPM_EXECUTABLE} run build
    WORKING_DIRECTORY ${ui_dir}
    USES_TERMINAL
    VERBATIM
  )
endfunction()
