#pragma once
#include <array>
#include <cstdint>
#include <functional>

// ─── Flags register bit positions ────────────────────────────────────────────
// Bit layout: S Z 0 AC 0 P 1 CY
//              7 6 5  4 3 2 1  0
static constexpr uint8_t FLAG_CY = 0x01;   // Carry
static constexpr uint8_t FLAG_P  = 0x04;   // Parity
static constexpr uint8_t FLAG_AC = 0x10;   // Auxiliary Carry
static constexpr uint8_t FLAG_Z  = 0x40;   // Zero
static constexpr uint8_t FLAG_S  = 0x80;   // Sign
// Bits 1, 3, 5 have fixed values on the 8080: bit1=1, bit3=0, bit5=0
static constexpr uint8_t FLAG_FIXED = 0x02;  // bit 1 always set

// ─── Machine state ────────────────────────────────────────────────────────────
struct State8080 {
    // 8-bit general-purpose registers
    uint8_t A{0};
    uint8_t B{0}, C{0};
    uint8_t D{0}, E{0};
    uint8_t H{0}, L{0};

    // Flags register (S Z 0 AC 0 P 1 CY)
    uint8_t F{FLAG_FIXED};

    // 16-bit special registers
    uint16_t PC{0};
    uint16_t SP{0};

    // 64 KB address space
    std::array<uint8_t, 0x10000> mem{};

    // Interrupt enable flip-flop
    bool inte{false};
    bool halted{false};

    // ── Flag helpers ──────────────────────────────────────────────────────────
    bool flag_cy() const { return (F & FLAG_CY) != 0; }
    bool flag_p()  const { return (F & FLAG_P)  != 0; }
    bool flag_ac() const { return (F & FLAG_AC) != 0; }
    bool flag_z()  const { return (F & FLAG_Z)  != 0; }
    bool flag_s()  const { return (F & FLAG_S)  != 0; }

    void set_cy(bool v) { v ? (F |= FLAG_CY) : (F &= ~FLAG_CY); }
    void set_p (bool v) { v ? (F |= FLAG_P)  : (F &= ~FLAG_P);  }
    void set_ac(bool v) { v ? (F |= FLAG_AC) : (F &= ~FLAG_AC); }
    void set_z (bool v) { v ? (F |= FLAG_Z)  : (F &= ~FLAG_Z);  }
    void set_s (bool v) { v ? (F |= FLAG_S)  : (F &= ~FLAG_S);  }

    // ── Register-pair helpers ─────────────────────────────────────────────────
    uint16_t BC() const { return (uint16_t(B) << 8) | C; }
    uint16_t DE() const { return (uint16_t(D) << 8) | E; }
    uint16_t HL() const { return (uint16_t(H) << 8) | L; }

    void setBC(uint16_t v) { B = v >> 8; C = v & 0xFF; }
    void setDE(uint16_t v) { D = v >> 8; E = v & 0xFF; }
    void setHL(uint16_t v) { H = v >> 8; L = v & 0xFF; }

    // PSW = FLAGS:A packed as a 16-bit word (used by PUSH PSW / POP PSW)
    uint16_t PSW() const { return (uint16_t(A) << 8) | F; }
    void setPSW(uint16_t v) { A = v >> 8; F = (v & 0xFF) | FLAG_FIXED; }

    // ── Memory helpers ────────────────────────────────────────────────────────
    uint8_t  read8 (uint16_t addr)  const { return mem[addr]; }
    uint16_t read16(uint16_t addr)  const {
        return uint16_t(mem[addr]) | (uint16_t(mem[addr + 1]) << 8);
    }
    void write8 (uint16_t addr, uint8_t  v) { mem[addr] = v; }
    void write16(uint16_t addr, uint16_t v) {
        mem[addr]     = v & 0xFF;
        mem[addr + 1] = v >> 8;
    }

    // Inline fetch helpers (advance PC)
    uint8_t  next8()  { return mem[PC++]; }
    uint16_t next16() {
        uint8_t lo = mem[PC++];
        uint8_t hi = mem[PC++];
        return uint16_t(lo) | (uint16_t(hi) << 8);
    }

    // Stack helpers
    void push16(uint16_t v) { SP -= 2; write16(SP, v); }
    uint16_t pop16()        { uint16_t v = read16(SP); SP += 2; return v; }
};

// ─── I/O callbacks ────────────────────────────────────────────────────────────
// Provide your own implementations or leave as nullptr for unimplemented ports.
struct IOBus {
    std::function<uint8_t(uint8_t port)>            in_handler;
    std::function<void  (uint8_t port, uint8_t val)> out_handler;
};

// ─── Public API ───────────────────────────────────────────────────────────────

// Execute one instruction; returns the number of clock cycles consumed.
int Step8080(State8080& state, IOBus& io);

// Load a binary image into memory starting at `offset`.
void LoadBinary(State8080& state, const char* path, uint16_t offset = 0x0000);
