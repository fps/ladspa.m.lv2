.PHONY: all

all:
	CXXFLAGS="-fPIC" ./waf configure
	CXXFLAGS="-fPIC" ./waf -v

