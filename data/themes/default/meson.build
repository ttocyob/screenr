procvue_edj = custom_target('screenr.edj',
  input        : 'screenr.edc',
  output       : 'screenr.edj',
  command      : [
    edje_cc,
    '-id', meson.current_source_dir(),
    '-fd', meson.current_source_dir(),
    '@INPUT@', '@OUTPUT@',
  ],
  build_by_default : true,
  install          : true,
  install_dir      : get_option('prefix') / get_option('datadir') / 'screenr',
)

install_data(
  'images/done_outline.png',
  'images/handle.png',
  install_dir : get_option('prefix') / get_option('datadir') / 'screenr' / 'images',
)
