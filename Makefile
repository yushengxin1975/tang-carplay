# Makefile for PC host build (unit tests + future host-mode runtime)
# Tool: MSYS2 / MinGW64 g++ (Windows). On Linux use system g++.
# Used to validate protocol layer compiles & behaves correctly on PC.
#
# Usage:
#   make            # build test.exe
#   make run        # build + run unit tests
#   make clean

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Iclient/include -Iclient/cpp

# Filled source-paths (relative to repo root)
SRC_DIRS := client/cpp
INCLUDE := client/include

SRC := $(wildcard client/cpp/protocol/*.cpp) \
      $(wildcard client/cpp/media/*.cpp) \
      $(wildcard client/test/*.cpp)

# Headless: avoid pulling in libusbdotnet. Tests use FakeTransport.
OBJ := $(SRC:.cpp=.o)
BIN := test.exe

.PHONY: all run clean

all: $(BIN)

run: $(BIN)
	@./$(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)