// thirdparty/fakese/main.cpp — A fake shell program to test the exec shim ability.
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    printf("Hello, this is not real sh!\n");
    printf("Enter \'quit\' to exit.\n");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") {
            break;
        }
        std::cout << line << std::endl;
    }

    return 0;
}
