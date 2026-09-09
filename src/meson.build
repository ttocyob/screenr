version_h = vcs_tag(
  input    : 'screenr_version.h.in',
  output   : 'screenr_version.h',
  fallback : meson.project_version(),
)

executable('screenr',
  sources      : files(
    'main.c',
    'callbacks.c',
    'recorder.c',
    'capture_preview.c',
    'overlay.c',
    'overlay_body_drag.c',
    'overlay_fill_sync.c',
    'overlay_handle_hit_ext.c',
    'overlay_done_button.c',
  ) + [version_h],
  dependencies : deps,
  c_args       : ['-DSCREENR_DATADIR="' + (get_option('prefix') / get_option('datadir') / 'screenr') + '"'],
  install      : true,
  install_dir  : get_option('bindir'),
)
