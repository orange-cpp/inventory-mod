#include "game/infection_sounds.hpp"

namespace game::detail
{
    const std::vector<InsaneVoice>& insane_voices()
    {
        static const std::vector<InsaneVoice> voices = {
            {"coach",
             {"Coach_Laughter01", "Coach_Laughter04", "Coach_Laughter06", "Coach_Laughter07", "Coach_Laughter13",
              "Coach_Laughter14", "Coach_Laughter22"},
             {"Coach_DeathScream01", "Coach_DeathScream02", "Coach_DeathScream03", "Coach_DeathScream04",
              "Coach_DeathScream05", "Coach_ScreamWhilePounced01", "Coach_ScreamWhilePounced03",
              "Coach_ScreamWhilePounced05"}},
            {"gambler",
             {"Gambler_Laughter01", "Gambler_Laughter03", "Gambler_Laughter06", "Gambler_Laughter15",
              "Gambler_Laughter16", "Gambler_Laughter17"},
             {"Gambler_DeathScream01", "Gambler_DeathScream02", "Gambler_DeathScream03", "Gambler_DeathScream05",
              "Gambler_ScreamWhilePounced01", "Gambler_ScreamWhilePounced03", "Gambler_ScreamWhilePounced05"}},
            {"mechanic",
             {"Mechanic_Laughter04", "Mechanic_Laughter05", "Mechanic_Laughter06", "Mechanic_Laughter09",
              "Mechanic_Laughter13b", "Mechanic_Laughter13c", "Mechanic_Laughter14"},
             {"Mechanic_DeathScream01", "Mechanic_DeathScream02", "Mechanic_DeathScream03", "Mechanic_DeathScream04",
              "Mechanic_ScreamWhilePounced01", "Mechanic_ScreamWhilePounced04", "Mechanic_ScreamWhilePounced04a"}},
            {"producer",
             {"Producer_Laughter01", "Producer_Laughter04", "Producer_Laughter12", "Producer_Laughter13",
              "Producer_Laughter14", "Producer_Laughter17"},
             {"Producer_DeathScream01", "Producer_DeathScream02", "Producer_ScreamWhilePounced01",
              "Producer_ScreamWhilePounced02", "Producer_ScreamWhilePounced03", "Producer_ScreamWhilePounced05"}},
            {"biker",
             {"npc.Biker_Laughter01", "npc.Biker_Laughter02", "npc.Biker_Laughter04", "npc.Biker_Laughter05",
              "npc.Biker_Laughter06", "npc.Biker_Laughter12", "npc.Biker_Laughter13"},
             {"Player.Biker_DeathScream06", "Player.Biker_DeathScream07", "Player.Biker_DeathScream08",
              "Player.Biker_DeathScream09", "Player.Biker_ScreamWhilePounced01", "Player.Biker_ScreamWhilePounced02",
              "Player.Biker_ScreamWhilePounced04"}},
            {"manager",
             {"npc.Manager_Laughter01", "npc.Manager_Laughter02", "npc.Manager_Laughter04", "npc.Manager_Laughter05",
              "npc.Manager_Laughter13", "npc.Manager_Laughter14", "npc.Manager_Laughter20"},
             {"Player.Manager_DeathScream02", "Player.Manager_DeathScream03", "Player.Manager_DeathScream04",
              "Player.Manager_DeathScream06", "Player.Manager_ScreamWhilePounced01", "Player.Manager_ScreamWhilePounced02"}},
            {"namvet",
             {"npc.NamVet_Laughter01", "npc.NamVet_Laughter02", "npc.NamVet_Laughter04", "npc.NamVet_Laughter05",
              "npc.NamVet_Laughter06", "npc.NamVet_Laughter07"},
             {"Player.NamVet_DeathScream01", "Player.NamVet_DeathScream03", "Player.NamVet_DeathScream04",
              "Player.NamVet_DeathScream05", "Player.NamVet_ScreamWhilePounced01", "Player.NamVet_ScreamWhilePounced03"}},
            {"teenangst",
             {"npc.TeenGirl_Laughter02", "npc.TeenGirl_Laughter04", "npc.TeenGirl_Laughter05", "npc.TeenGirl_Laughter09",
              "npc.TeenGirl_Laughter10", "npc.TeenGirl_Laughter12", "npc.TeenGirl_Laughter13"},
             {"Player.TeenGirl_DeathScream05", "Player.TeenGirl_DeathScream07", "Player.TeenGirl_DeathScream09",
              "Player.TeenGirl_DeathScream10", "Player.TeenGirl_ScreamWhilePounced01",
              "Player.TeenGirl_ScreamWhilePounced02", "Player.TeenGirl_ScreamWhilePounced06"}},
        };
        return voices;
    }
}
