CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
INCLUDES := -Iinclude
BUILD_DIR := build
TARGET := $(BUILD_DIR)/minisql

SRCS := src/main.cpp
OBJS := $(SRCS:src/%.cpp=$(BUILD_DIR)/%.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) data

test: all
	@echo "Run: ./$(TARGET) data -f tests/demo.sql"
