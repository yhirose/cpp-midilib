CXXFLAGS = -std=c++17 -Wall -Wextra -I..

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  LDLIBS = -framework CoreMIDI -framework CoreFoundation
  TESTS = test_common test
else ifeq ($(UNAME),Linux)
  LDLIBS = -ldl -lpthread
  TESTS = test_common
else
  LDLIBS = -lwinmm
  TESTS = test_common
endif

all: example bridge $(TESTS)
	for t in $(TESTS); do ./$$t || exit 1; done

example: ../midilib.h example.cpp
	$(CXX) $(CXXFLAGS) -o example example.cpp $(LDLIBS)

bridge: ../midilib.h bridge.cpp
	$(CXX) $(CXXFLAGS) -o bridge bridge.cpp $(LDLIBS)

test: ../midilib.h test.cpp
	$(CXX) $(CXXFLAGS) -o test test.cpp $(LDLIBS)

test_common: ../midilib.h test_common.cpp
	$(CXX) $(CXXFLAGS) -o test_common test_common.cpp $(LDLIBS)

clean:
	rm -f example bridge test test_common
