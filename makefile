.PHONY: all

all:
	CXXFLAGS="-fPIC -Wall -O3 -march=native" ./waf configure
	CXXFLAGS="-fPIC -Wall -O3 -march=native" ./waf -v

