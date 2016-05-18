.PHONY: all install clean

#all: ladspa.m.lv2/synth.so ladspa.m.lv2/instrument.so
all: ladspa.m.lv2/instrument.so

CXXFLAGS=-Wall -O3 -march=native `pkg-config ladspa.m-1 ladspa.m.proto-1`

ladspa.m.lv2/instrument.so:
	g++ $(CXXFLAGS) -o ladspa.m.lv2/instrument.so -shared -fPIC instrument.cc

install:
	./waf install

clean:
	./waf clean

