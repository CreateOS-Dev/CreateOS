#include "security/process.h"
#include "drivers/tty.h"

static uint64_t process_generations[TTY_CONSOLE_COUNT];

uint64_t security_process_generation(int console)
{
    if (console < 0 || console >= TTY_CONSOLE_COUNT)
        return 0;
    if (!process_generations[console])
        process_generations[console] = 1;
    return process_generations[console];
}

int security_process_generation_matches(int console, uint64_t generation)
{
    return generation &&
           security_process_generation(console) == generation;
}

int security_advance_process_generation(int console)
{
    uint64_t generation = security_process_generation(console);
    if (!generation || generation >= SECURITY_PROCESS_GENERATION_MAX)
        return 0;
    process_generations[console] = generation + 1;
    return 1;
}
