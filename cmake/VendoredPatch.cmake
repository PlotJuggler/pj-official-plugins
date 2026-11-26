# Applies a source-text patch to a vendored (CPM-downloaded) tarball at configure time,
# without a `patch` executable (Windows CI). Used for upstream bug fixes and hardening guards.
#
# pj_apply_vendored_patch(<content_var> <anchor_var> <fixed_var> <label>)
#   - content_var, anchor_var, fixed_var are passed BY NAME (not value): the
#     patched C++ contains ';' which CMake would split if passed as arguments.
#   - Replaces the (unique) <anchor> text in <content_var> with <fixed>, in place.
#   - FATAL_ERROR if the anchor is absent, so a moved pin can never silently drop
#     the fix. <label> names the patch in that error.
function(pj_apply_vendored_patch content_var anchor_var fixed_var label)
  string(FIND "${${content_var}}" "${${anchor_var}}" _anchor_pos)
  if(_anchor_pos EQUAL -1)
    message(FATAL_ERROR "${label}: vendored-patch anchor no longer applies — re-check the fix against the pinned source")
  endif()
  string(REPLACE "${${anchor_var}}" "${${fixed_var}}" _patched "${${content_var}}")
  set(${content_var} "${_patched}" PARENT_SCOPE)
endfunction()
