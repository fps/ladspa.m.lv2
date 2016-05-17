#!/usr/bin/env python
from waflib.extras import autowaf as autowaf
import re

# Variables for 'waf dist'
APPNAME = 'ladspa.m.lv2'
VERSION = '1.0.0'

# Mandatory variables
top = '.'
out = 'build'

def options(opt):
    opt.load('compiler_cxx')
    autowaf.set_options(opt)

def configure(conf):
    conf.load('compiler_cxx')
    autowaf.configure(conf)
    autowaf.set_c99_mode(conf)
    autowaf.display_header('Sampler Configuration')

    if not autowaf.is_child():
        autowaf.check_pkg(conf, 'lv2', atleast_version='1.1.0', uselib_store='LV2')

    autowaf.check_pkg(conf, 'ladspa.m-1', uselib_store='LADSPAM',
                      atleast_version='0.0.1', mandatory=True)
    autowaf.check_pkg(conf, 'gtk+-2.0', uselib_store='GTK2',
                      atleast_version='2.18.0', mandatory=False)

    autowaf.display_msg(conf, 'LV2 bundle directory', conf.env.LV2DIR)
    print('')

def build(bld):
    bundle = 'ladspa.m.lv2'

    # Make a pattern for shared objects without the 'lib' prefix
    module_pat = re.sub('^lib', '', bld.env.cxxshlib_PATTERN)
    module_ext = module_pat[module_pat.rfind('.'):]

    # Build manifest.ttl by substitution (for portable lib extension)
    bld(features     = 'subst',
        source       = 'manifest.ttl.in',
        target       = '%s/%s' % (bundle, 'manifest.ttl'),
        install_path = '${LV2DIR}/%s' % bundle,
        LIB_EXT      = module_ext)
    
    # Copy other data files to build bundle (build/eg-sampler.lv2)
    for i in ['instrument.ttl']:
        bld(features     = 'subst',
            is_copy      = True,
            source       = i,
            target       = '%s/%s' % (bundle, i),
            install_path = '${LV2DIR}/%s' % bundle)

    # Use LV2 headers from parent directory if building as a sub-project
    includes = ['.']
    if autowaf.is_child:
        includes += ['../..']

    # Build plugin library
    obj = bld(features     = 'cxx cxxshlib',
              source       = 'instrument.cc',
              name         = 'instrument',
              target       = '%s/instrument' % bundle,
              install_path = '${LV2DIR}/%s' % bundle,
              use          = 'SNDFILE LV2 LADSPAM',
              includes     = includes,
              lib          = 'ladspa.m.proto-1')
              #includes     = includes,
              #lib          = 'ladspam.pb')
    obj.env.cxxshlib_PATTERN = module_pat

