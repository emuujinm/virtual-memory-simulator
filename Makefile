CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS ?= -Iinclude
TARGET := sim
SRC := src/main.cpp

.PHONY: all clean test demo
all: $(TARGET)

$(TARGET): $(SRC) $(wildcard include/*.hpp)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SRC) -o $@

test: $(TARGET)
	./$(TARGET) --test

demo: $(TARGET)
	./$(TARGET) --demo

clean:
	rm -f $(TARGET)
