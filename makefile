.PHONY: all

all:
	CXXFLAGS="-fPIC -Wall" ./waf configure
	CXXFLAGS="-fPIC -Wall" ./waf -v

