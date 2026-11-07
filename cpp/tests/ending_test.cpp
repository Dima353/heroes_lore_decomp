#include "ending.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        EndingSequence ending;
        ending.start({"\"Are you ready?\"", "_32", "I heard", "the voice", "_48"}, {"MANAGER", "-", "KO", "=", "END"});
        check(ending.fade_level() == 255, "as.d starts from white");
        for (int i = 0; i < 16; ++i) ending.tick(240);
        check(ending.fade_level() == -1, "sixteen frames fade to black");
        ending.tick(240);
        check(ending.first == 0 && ending.end == 1 && ending.counter == 31, "first page runs up to the _32 marker");
        for (int i = 0; i < 50; ++i) ending.tick(240);
        check(ending.first == 0, "pages turn only on a key");
        ending.press();ending.tick(240);
        check(ending.first == 2 && ending.end == 4 && ending.counter == 47, "second page");
        ending.press();ending.tick(240);
        check(ending.phase == EndingSequence::Phase::credits, "the staff roll follows the last page");
        ending.tick(240);
        check(ending.rolling.size() == 1 && ending.rolling[0].y == 238, "a credit line enters at the bottom and rises 2px");
        int guard = 0;
        while (ending.phase != EndingSequence::Phase::done && ++guard < 1000) ending.tick(240);
        check(ending.phase == EndingSequence::Phase::done && guard < 200, "roll ends once every line has left the screen");
        std::cout << "Ending checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
