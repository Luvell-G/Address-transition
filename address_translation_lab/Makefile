CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic

all: sim

sim: src/main.cpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o sim

run: sim
	./sim

clean:
	rm -f sim