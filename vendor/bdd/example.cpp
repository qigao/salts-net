#include "bdd-for-c.h"
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

// Example C++ class to test
class Calculator {
public:
    int add(int a, int b) const { return a + b; }
    int subtract(int a, int b) const { return a - b; }
    int multiply(int a, int b) const { return a * b; }
    double divide(double a, double b) const { 
        if (b == 0.0) throw std::runtime_error("Division by zero");
        return a / b; 
    }
};

// Example with STL containers
class StringProcessor {
public:
    std::string reverse(const std::string& str) {
        return std::string(str.rbegin(), str.rend());
    }
    
    std::vector<std::string> split(const std::string& str, char delimiter) {
        std::vector<std::string> result;
        std::string current;
        for (char c : str) {
            if (c == delimiter) {
                if (!current.empty()) {
                    result.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            result.push_back(current);
        }
        return result;
    }
};

spec("C++ BDD Example") {
    static Calculator calc;
    static StringProcessor processor;

    describe("Calculator") {
        it("should add two numbers") {
            int result = calc.add(2, 3);
            check(result == 5);
        }

        it("should subtract two numbers") {
            int result = calc.subtract(10, 3);
            check(result == 7);
        }

        it("should multiply two numbers") {
            int result = calc.multiply(4, 5);
            check(result == 20);
        }

        it("should divide two numbers") {
            double result = calc.divide(10.0, 2.0);
            check(result == 5.0);
        }
    }

    describe("StringProcessor") {
        it("should reverse a string") {
            std::string input = "hello";
            std::string result = processor.reverse(input);
            check(result == "olleh");
        }

        it("should split string by delimiter") {
            std::string input = "one,two,three";
            std::vector<std::string> result = processor.split(input, ',');
            check(result.size() == 3);
            check(result[0] == "one");
            check(result[1] == "two");
            check(result[2] == "three");
        }

        it("should handle empty string") {
            std::string input = "";
            std::string result = processor.reverse(input);
            check(result == "");
        }
    }

    describe("Setup and Teardown Hooks") {
        static int counter;
        static std::vector<std::string> log;

        before_each() {
            // This runs before each test in this describe block
            counter = 0;
            log.clear();
            log.push_back("setup");
        }

        after_each() {
            // This runs after each test in this describe block
            log.push_back("teardown");
        }

        it("should have fresh state from before_each") {
            check(counter == 0);
            check(log.size() == 1);
            check(log[0] == "setup");
            counter = 42;
            log.push_back("test1");
        }

        it("should reset state for each test") {
            // Counter should be reset to 0, not 42 from previous test
            check(counter == 0);
            check(log.size() == 1);
            check(log[0] == "setup");
            log.push_back("test2");
        }
    }

    describe("Modern C++ Features") {
        it("should work with smart pointers") {
            auto ptr = std::make_unique<Calculator>();
            int result = ptr->add(5, 7);
            check(result == 12);
        }

        it("should work with lambdas") {
            auto add_lambda = [](int a, int b) { return a + b; };
            int result = add_lambda(3, 4);
            check(result == 7);
        }

        it("should work with range-based for loops") {
            std::vector<int> numbers = {1, 2, 3, 4, 5};
            int sum = 0;
            for (int n : numbers) {
                sum += n;
            }
            check(sum == 15);
        }
    }
}
