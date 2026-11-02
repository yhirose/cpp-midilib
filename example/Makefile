CXXFLAGS = -std=c++17 -Wall -Wextra -I..
LDLIBS = -framework CoreMIDI -framework CoreFoundation

all: example bridge test
	./test

bridge: ../midilib.h bridge.cpp
	$(CXX) $(CXXFLAGS) -o bridge bridge.cpp $(LDLIBS)

example: ../midilib.h example.cpp
	$(CXX) $(CXXFLAGS) -o example example.cpp $(LDLIBS)

test: ../midilib.h test.cpp
	$(CXX) $(CXXFLAGS) -o test test.cpp $(LDLIBS)

clean:
	rm -f example test
