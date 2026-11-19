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
// The registry's {u16 id; UTF-16 name[6]} entry name for an id, as 82481BE8
// returns it (0 when the id has no entry).
uint32_t RegistryName(uint8_t* base, uint32_t id)
{
    const uint32_t count = PPC_LOAD_U8(settings::language::Registry + 416);
    for (uint32_t i = 0; i < count; ++i)
        if (PPC_LOAD_U16(settings::language::Registry + i * 14) == id) return settings::language::Registry + i * 14 + 2;
    return 0;
}
// The id whose registry name is at this address (0 when none is).
uint32_t RegistryId(uint8_t* base, uint32_t name)
{
    const uint32_t count = PPC_LOAD_U8(settings::language::Registry + 416);
    for (uint32_t i = 0; i < count; ++i)
        if (settings::language::Registry + i * 14 + 2 == name) return PPC_LOAD_U16(settings::language::Registry + i * 14);
    return 0;
}
}
// 82481E78(voice language NAME, a UTF-16 pointer) maps the name through the
// registry and copies the voice language's name to the cache at 0x83318000,
// which 82481F40 turns back into the voice id the game streams. The retail
// config apply (82870E38) passes 82482038(save's voice index), i.e. a registry
// name pointer; the first-use init (82481EE8) passes the built-in default name.
// A Voice language chosen in Settings (settings.ini voice_language) replaces
// that name with the chosen id's registry name, so voices stay independent of
// the save and of the text language.
PPC_FUNC(sub_82481E78)
{
    const auto caller = uint32_t(ctx.lr);
    const uint32_t chosen = settings::GetConfig().voiceLanguage;
    if (chosen && VoiceListed(base, chosen))
    {
        const uint32_t name = RegistryName(base, chosen);
        if (name && name != ctx.r3.u32)
        {
            static uint32_t logged = 0;
            if (logged != chosen)
            {
                logged = chosen;
                LOG_INFO("language: voices in language {} (Settings), the game asked for name {:#x} (language {})",
                    chosen, ctx.r3.u32, RegistryId(base, ctx.r3.u32));
            }
            ctx.r3.u64 = name;
        }
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
