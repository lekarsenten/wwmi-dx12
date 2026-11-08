#include "diag_arms.h"

DiagArms g_diag_arms;

void DiagArms::Tick()
{
    {
        int s = log_all_draws.load();
        if (s > 0) log_all_draws.store(s - 1);
    }
    {
        int s = texflip_arm.load();
        if (s > 0) texflip_arm.store(s - 1);
    }
    {
        int s = texnoswap_arm.load();
        if (s > 0) texnoswap_arm.store(s - 1);
    }
    {
        int s = log_merged.load();
        if (s > 0) log_merged.store(s - 1);
    }
}
