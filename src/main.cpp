#include "cpu8080.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

// ─── CP/M BIOS hook ───────────────────────────────────────────────────────────
// Inject a RET at address 0x0005 so CP/M programs that CALL 5 come back.
// Before each step we intercept PC == 0x0005 with register C checked here.
static bool cpm_bdos(State8080& s) {
    if (s.PC != 0x0005) return false;

    switch (s.C) {
        case 2: {
            // BDOS function 2: console character output (char in E)
            std::putchar(s.E);
            break;
        }
        case 9: {
            // BDOS function 9: print string at DE, terminated by '$'
            uint16_t addr = s.DE();
            while (s.mem[addr] != '$') {
                std::putchar(s.mem[addr++]);
            }
            std::putchar('\n');
            break;
        }
        default:
            // Other BDOS calls are silently ignored
            break;
    }

    // Simulate RET: pop return address from stack
    s.PC = s.pop16();
    return true;
}

// ─── I/O bus setup ────────────────────────────────────────────────────────────
// Extend these handlers to wire real peripherals.
static IOBus make_io_bus() {
    IOBus io;

    io.in_handler = [](uint8_t port) -> uint8_t {
        std::fprintf(stderr, "[IO] IN  port 0x%02X -> 0xFF (unimplemented)\n", port);
        return 0xFF;
    };

    io.out_handler = [](uint8_t port, uint8_t val) {
        std::fprintf(stderr, "[IO] OUT port 0x%02X <- 0x%02X\n", port, val);
    };

    return io;
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <program.com> [load_offset_hex]\n", argv[0]);
        std::fprintf(stderr, "  load_offset_hex defaults to 0100 (standard CP/M load address)\n");
        return 1;
    }

    // Optional second argument: hex load offset (default 0x0100 for CP/M .COM)
    uint16_t load_offset = 0x0100;
    if (argc >= 3) {
        load_offset = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 16));
    }

    State8080 state;
    IOBus     io = make_io_bus();

    // ── CP/M compatibility setup ──────────────────────────────────────────────
    // Warm-boot vector: CALL 0x0000 at the start of the CP/M stack area
    // Place a HLT at 0x0000 so reaching it terminates cleanly
    state.mem[0x0000] = 0x76;   // HLT  — fall-through safety

    // 0x0005 must be reachable as a CALL target for BDOS; we'll intercept
    // it via cpm_bdos() before the CPU sees it.  Put a RET there anyway so
    // a raw (unhooked) call still returns gracefully.
    state.mem[0x0005] = 0xC9;   // RET

    // Set CP/M default stack (just below the 64-KB top)
    state.SP = 0xF000;

    try {
        LoadBinary(state, argv[1], load_offset);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Load error: %s\n", e.what());
        return 1;
    }

    // Start execution at the CP/M program load address
    state.PC = load_offset;

    std::fprintf(stderr, "Native8080: loaded '%s' at 0x%04X, running...\n",
                 argv[1], load_offset);

    // ── Main execution loop ───────────────────────────────────────────────────
    for (;;) {
        // CP/M BDOS hook — intercept before fetch
        if (cpm_bdos(state)) continue;

        // Exit on HALT or when PC wraps to 0x0000 (warm-boot)
        if (state.halted || state.PC == 0x0000) break;

        Step8080(state, io);
    }

    std::fprintf(stderr, "\nNative8080: CPU halted. PC=0x%04X\n", state.PC);
    return 0;
}
