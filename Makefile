CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
BUILD := build

all: $(BUILD)/parser-test

$(BUILD)/parser-test: tests/main.cpp src/html_parser.h src/html_parser.ipp src/ir.h src/charset.h src/charset.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/main.cpp src/charset.cpp

test: $(BUILD)/parser-test
	./$(BUILD)/parser-test

# autonomous loop over fixtures + common sites
loop: $(BUILD)/parser-test
	./$(BUILD)/parser-test tests/fixtures/simple.html
	./$(BUILD)/parser-test tests/fixtures/forms.html
	./$(BUILD)/parser-test tests/fixtures/messy.html
	./$(BUILD)/parser-test tests/fixtures/cyrillic.html
	@for u in https://www.bbc.com/news https://habr.com/ru/articles/; do \
		echo "=== $u"; ./$(BUILD)/parser-test "$$u" | head -30; done

.PHONY: all test loop
