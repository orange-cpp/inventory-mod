#pragma once

#include <vector>

namespace game::detail
{
    struct InsaneVoice
    {
        const char* model;
        std::vector<const char*> laughs, screams;
    };

    const std::vector<InsaneVoice>& insane_voices();
}
