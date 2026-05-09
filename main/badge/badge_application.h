#pragma once

#include "badge_board.h"

class BadgeApplication {
public:
    void Initialize();
    void Run();

private:
    BadgeBoard board_;
};
