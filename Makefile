CXX ?= c++
CXXFLAGS ?= -std=c++17 -O3 -march=native -DNDEBUG -Wall -Wextra -pedantic
TARGET := artemis_ii_met
SRC := artemis_ii_met.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
