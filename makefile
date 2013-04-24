.PHONY: all install

all:
	CXXFLAGS="-fPIC -Wall -O3 -march=native -msse -mfpmath=sse" ./waf configure
	CXXFLAGS="-fPIC -Wall -O3 -march=native -msse -mfpmath=sse" ./waf -v

install:
	./waf install
