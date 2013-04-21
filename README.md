# ladspa.m.lv2

LV2 host(s) for loading ladspa.m.proto definition(s)

# What?

This is a simple LV2 plugin that can load ladspa.m.proto instrument definition files. Check out

http://github.com/fps/ladspa.m.proto

ladspa.m.proto is a google protobuf definition for a file format for describing modular synthesis graphs made up out of LADSPA plugins. ladspa.m.lv2 uses the library ladspa.m

http://github.com/fps/ladspa.m

to implement the concrete synthesis graph defined by the protbuf file.

# How to use it?

Create an instrument protobuf file (checkout the example python script for ladspa.m.proto).

Add an instance of ladspa.m.lv2 to a host of your choice. Ardour3 and QTractor are some examples:

http://qtractor.sourceforge.net/qtractor-index.html

http://ardour.org/

Then open the configuration for the ladspa.m.lv2 instance and load the previously created instrument definition protobuf file.

# Requirements

* ladspa.m.proto

* ladspa.m

* lv2 + midi extensions + gui extension

# Author

Florian Paul Schmidt

# License 

GNU LGPL version 3.0 or higher.

