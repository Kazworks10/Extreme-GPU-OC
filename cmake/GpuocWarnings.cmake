# Project warning policy. Applied per target, never globally, so third-party
# code built in-tree (GoogleTest) keeps its own flags.
function(gpuoc_set_warnings target)
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang-cl: "-Wall" means -Weverything under the cl driver, so the clang
    # flags are passed through /clang: to get the real -Wall semantics.
    target_compile_options(${target} PRIVATE
      /clang:-Wall /clang:-Wextra /clang:-Wpedantic
      $<$<BOOL:${GPUOC_WERROR}>:/clang:-Werror>)
  elseif(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 $<$<BOOL:${GPUOC_WERROR}>:/WX>
      /permissive- /Zc:__cplusplus /utf-8
      /external:anglebrackets /external:W0)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic
      $<$<BOOL:${GPUOC_WERROR}>:-Werror>)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # GCC 12/13 report false positives for std::variant/std::optional
      # members at -O2+ (GCC bug 105562 and related). Development hosts only.
      target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
    endif()
  endif()

  if(WIN32)
    target_compile_definitions(${target} PRIVATE
      UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  endif()
endfunction()
