execute_process(
  COMMAND west patch -sm zmk-feature-split-esb clean
  WORKING_DIRECTORY ${WEST_TOPDIR}
  OUTPUT_QUIET ERROR_QUIET
)
execute_process(
  COMMAND west patch -sm zmk-feature-split-esb apply
  WORKING_DIRECTORY ${WEST_TOPDIR}
  RESULT_VARIABLE patch_result
)
if(NOT patch_result EQUAL 0)
  message(FATAL_ERROR "west patch apply for zmk-feature-split-esb failed (${patch_result})")
endif()
