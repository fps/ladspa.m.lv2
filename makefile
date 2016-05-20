.PHONY: all install clean

#all: ladspa.m.lv2/synth.so ladspa.m.lv2/instrument.so
all: bld/ladspa.m.lv2/instrument.so

CXXFLAGS=-Wall -O3 -march=native
DEPFLAGS= `pkg-config --cflags --libs ladspa.m-1 ladspa.m.proto-1`

bld/ladspa.m.lv2/instrument.so:
	g++ $(CXXFLAGS) -o bld/ladspa.m.lv2/instrument.so -shared -fPIC instrument.cc ${DEPFLAGS}

install:
	./waf install

clean:
	rm -f bld/ladspa.m.lv2/instrument.so bld/ladspa.m.lva/synth.so

