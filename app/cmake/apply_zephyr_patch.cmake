# west patch supplies the absolute patch path as the final argument and runs
# here in the destination module. Never discard that checkout's local edits.
if(NOT CMAKE_ARGC EQUAL 4)
  message(FATAL_ERROR "Usage: cmake -P apply_zephyr_patch.cmake PATCH")
endif()
set(patch "${CMAKE_ARGV3}")
execute_process(COMMAND git rev-parse --absolute-git-dir
                OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY)
# Multiple build directories can check/apply the same patch concurrently.
file(LOCK "${git_dir}/hispec-patch.lock" TIMEOUT 30)
execute_process(COMMAND git apply --reverse --check "${patch}"
                RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(applied EQUAL 0)
  return()
endif()
execute_process(COMMAND git apply --check "${patch}"
                RESULT_VARIABLE applicable OUTPUT_QUIET ERROR_VARIABLE reason)
if(NOT applicable EQUAL 0)
  message(FATAL_ERROR
    "Required Zephyr patch is neither applicable nor already applied: ${patch}\n"
    "Local changes were preserved. Reconcile this patch with the checkout.\n${reason}")
endif()
execute_process(COMMAND git apply "${patch}" COMMAND_ERROR_IS_FATAL ANY)
