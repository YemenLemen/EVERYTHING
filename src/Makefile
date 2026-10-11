CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
NC_CFLAGS := $(shell pkg-config --cflags notcurses)
NC_LIBS   := $(shell pkg-config --libs notcurses)

deckctl: main.cpp sysinfo.cpp sysinfo.hpp
	$(CXX) $(CXXFLAGS) $(NC_CFLAGS) main.cpp sysinfo.cpp -o $@ $(NC_LIBS)

clean:
	rm -f deckctl

.PHONY: clean
