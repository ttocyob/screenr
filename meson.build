project('screenr', 'c', 
        version : '0.1.1')

efl_version = '>= 1.27.0'

cc = meson.get_compiler('c')

deps = [ dependency('ecore', version : efl_version),
         dependency('ecore-x', version : efl_version),
         dependency('ecore-evas', version : efl_version),
         dependency('edje', version : efl_version),
         dependency('eina', version : efl_version),
         dependency('elementary', version : efl_version),
         dependency('eet', version : efl_version),
         cc.find_library('m', required : true), ]

#### dir locations  
dir_prefix = get_option('prefix')
dir_bin    = join_paths(dir_prefix, get_option('bindir'))
dir_data   = join_paths(dir_prefix, get_option('datadir'))

edje_cc = find_program('edje_cc', required : true)

subdir('src')
subdir('data')