# Majesty 2 GEM3 Boost Build System
# Compiler: MinGW-w64 32-bit PE
CXX ?= i686-w64-mingw32-g++
CXXFLAGS ?= -shared -O3 -msse2 -mfpmath=sse -static-libgcc -static-libstdc++ -Wall -Wextra
INCLUDES ?= -Iinclude -Isrc

SRC := $(wildcard src/*.cpp)
DEF := src/d3d9.def
TARGET := build/d3d9.dll

.PHONY: all clean dirs

all: dirs $(TARGET)

dirs:
	mkdir -p build

$(TARGET): $(SRC) $(DEF)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SRC) $(DEF) -lkernel32 -luser32

clean:
	rm -rf build
