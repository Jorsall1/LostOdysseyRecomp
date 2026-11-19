#include <stdafx.h>
#include "language_trace.h"
#include "config.h"
extern "C" PPC_FUNC(__imp__sub_82481E78);
extern "C" PPC_FUNC(__imp__sub_82481F40);
namespace
{
// Whether the game's voice list (the registry 82482028/82482038 read) has this id.
bool VoiceListed(uint8_t* base, uint32_t id)
{
    const uint32_t count = settings::language::VoiceCount(PPC_LOAD_U8(settings::language::Registry + 419));
    for (uint32_t i = 0; i < count; ++i)
        if (PPC_LOAD_U16(settings::language::Registry + 288 + i * 2) == id) return true;
    return false;
}
}
// 82481E78(voice language id) selects the voice data the game streams; the
// retail config apply (82870E38) passes the id of the save's Voice language.
// A Voice language chosen in Settings (settings.ini voice_language) takes its
// place, so voices stay independent of the save and of the text language.
PPC_FUNC(sub_82481E78)
{
    const auto caller = uint32_t(ctx.lr);
    const uint32_t chosen = settings::GetConfig().voiceLanguage;
    if (chosen && chosen != ctx.r3.u32 && VoiceListed(base, chosen))
    {
        static uint32_t logged = 0;
        if (logged != chosen)
        {
            logged = chosen;
            LOG_INFO("language: voices in language {} (Settings), the save asked for {}", chosen, ctx.r3.u32);
        }
        ctx.r3.u64 = chosen;
    }
    __imp__sub_82481E78(ctx, base);
    settings::language::TraceEffective(base, "voice-applied", caller, 0);
}
PPC_FUNC(sub_82481F40)
{
    const auto caller = uint32_t(ctx.lr);
    __imp__sub_82481F40(ctx, base);
    settings::language::TraceEffective(base, "voice-id", caller, ctx.r3.u32);
}
