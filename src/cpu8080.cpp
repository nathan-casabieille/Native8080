#include "cpu8080.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// ─── Internal helpers ─────────────────────────────────────────────────────────

// Even parity: returns true when the number of set bits is even.
static bool parity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

// Update S, Z, P flags from an 8-bit result.
static void update_szp(State8080& s, uint8_t result) {
    s.set_s(result & 0x80);
    s.set_z(result == 0);
    s.set_p(parity(result));
}

// Full arithmetic flag update (S Z AC P CY) after an ADD/ADC-class operation.
// `full` is the 16-bit result, `lhs` and `rhs` are the original operands.
static void update_flags_add(State8080& s, uint16_t full, uint8_t lhs, uint8_t rhs, uint8_t carry_in = 0) {
    uint8_t result = uint8_t(full);
    update_szp(s, result);
    s.set_cy(full > 0xFF);
    s.set_ac(((lhs & 0x0F) + (rhs & 0x0F) + carry_in) > 0x0F);
}

// Full arithmetic flag update for SUB/SBB-class operations.
static void update_flags_sub(State8080& s, uint8_t lhs, uint8_t rhs, uint8_t borrow_in = 0) {
    uint16_t full   = uint16_t(lhs) - uint16_t(rhs) - uint16_t(borrow_in);
    uint8_t  result = uint8_t(full);
    update_szp(s, result);
    s.set_cy(full > 0xFF);          // borrow sets carry
    // AC: borrow from bit 4 (lower nibble)
    s.set_ac(((lhs & 0x0F) - (rhs & 0x0F) - borrow_in) < 0);
}

// ─── Register accessor by 3-bit SSS/DDD field ────────────────────────────────
// Returns a reference to the register named by the 3-bit field.
// Field 110 (M) resolves to memory[HL].
// NOTE: M-field accesses go through the mem array directly.

static uint8_t reg_read(State8080& s, uint8_t field) {
    switch (field & 0x07) {
        case 0: return s.B;
        case 1: return s.C;
        case 2: return s.D;
        case 3: return s.E;
        case 4: return s.H;
        case 5: return s.L;
        case 6: return s.mem[s.HL()];   // M
        case 7: return s.A;
    }
    return 0; // unreachable
}

static void reg_write(State8080& s, uint8_t field, uint8_t val) {
    switch (field & 0x07) {
        case 0: s.B = val; break;
        case 1: s.C = val; break;
        case 2: s.D = val; break;
        case 3: s.E = val; break;
        case 4: s.H = val; break;
        case 5: s.L = val; break;
        case 6: s.mem[s.HL()] = val; break; // M
        case 7: s.A = val; break;
    }
}

// ─── Register-pair accessors by 2-bit RP field ───────────────────────────────

static uint16_t rp_read(State8080& s, uint8_t rp) {
    switch (rp & 0x03) {
        case 0: return s.BC();
        case 1: return s.DE();
        case 2: return s.HL();
        case 3: return s.SP;
    }
    return 0;
}

static void rp_write(State8080& s, uint8_t rp, uint16_t val) {
    switch (rp & 0x03) {
        case 0: s.setBC(val); break;
        case 1: s.setDE(val); break;
        case 2: s.setHL(val); break;
        case 3: s.SP = val;   break;
    }
}

// PUSH/POP use RP=11 for PSW (FLAGS:A), not SP.
static uint16_t rp_read_psw(State8080& s, uint8_t rp) {
    if ((rp & 0x03) == 3) return s.PSW();
    return rp_read(s, rp);
}
static void rp_write_psw(State8080& s, uint8_t rp, uint16_t val) {
    if ((rp & 0x03) == 3) { s.setPSW(val); return; }
    rp_write(s, rp, val);
}

// ─── Condition evaluation by 3-bit CCC field ─────────────────────────────────
static bool condition(State8080& s, uint8_t ccc) {
    switch (ccc & 0x07) {
        case 0: return !s.flag_z();   // NZ
        case 1: return  s.flag_z();   // Z
        case 2: return !s.flag_cy();  // NC
        case 3: return  s.flag_cy();  // C
        case 4: return !s.flag_p();   // PO (parity odd)
        case 5: return  s.flag_p();   // PE (parity even)
        case 6: return !s.flag_s();   // P  (positive)
        case 7: return  s.flag_s();   // M  (minus)
    }
    return false;
}

// ─── Step8080 ─────────────────────────────────────────────────────────────────
// Returns the number of clock cycles consumed by the instruction.
int Step8080(State8080& s, IOBus& io) {
    if (s.halted) return 4;

    uint8_t opcode = s.next8();

    // Extract common bit-fields
    uint8_t ddd = (opcode >> 3) & 0x07;   // destination / RP / condition
    uint8_t sss = (opcode)      & 0x07;   // source
    uint8_t rp  = (opcode >> 4) & 0x03;   // register pair

    switch (opcode) {

    // ── NOP ──────────────────────────────────────────────────────────────────
    case 0x00:  // NOP
    case 0x08:  // *NOP (undocumented)
    case 0x10:
    case 0x18:
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38:
        return 4;

    // ── HLT ──────────────────────────────────────────────────────────────────
    case 0x76:
        s.halted = true;
        return 7;

    // ── MOV D,S  (01DDDSSS) ─────────────────────────────────────────────────
    // Entire block 0x40–0x7F except 0x76 (HLT)
    // Handled below the switch via the bit-pattern check.

    // ── MVI D,#  (00DDD110) ──────────────────────────────────────────────────
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E: {
        uint8_t imm = s.next8();
        reg_write(s, ddd, imm);
        return (ddd == 6) ? 10 : 7;  // MVI M,# costs 10
    }

    // ── LXI RP,# (00RP0001) ──────────────────────────────────────────────────
    case 0x01: case 0x11: case 0x21: case 0x31: {
        uint16_t imm = s.next16();
        rp_write(s, rp, imm);
        return 10;
    }

    // ── LDA a ────────────────────────────────────────────────────────────────
    case 0x3A: {
        uint16_t addr = s.next16();
        s.A = s.read8(addr);
        return 13;
    }

    // ── STA a ────────────────────────────────────────────────────────────────
    case 0x32: {
        uint16_t addr = s.next16();
        s.write8(addr, s.A);
        return 13;
    }

    // ── LHLD a ───────────────────────────────────────────────────────────────
    case 0x2A: {
        uint16_t addr = s.next16();
        s.L = s.read8(addr);
        s.H = s.read8(addr + 1);
        return 16;
    }

    // ── SHLD a ───────────────────────────────────────────────────────────────
    case 0x22: {
        uint16_t addr = s.next16();
        s.write8(addr,     s.L);
        s.write8(addr + 1, s.H);
        return 16;
    }

    // ── LDAX BC / LDAX DE ────────────────────────────────────────────────────
    case 0x0A: s.A = s.read8(s.BC()); return 7;
    case 0x1A: s.A = s.read8(s.DE()); return 7;

    // ── STAX BC / STAX DE ────────────────────────────────────────────────────
    case 0x02: s.write8(s.BC(), s.A); return 7;
    case 0x12: s.write8(s.DE(), s.A); return 7;

    // ── XCHG ─────────────────────────────────────────────────────────────────
    case 0xEB: {
        uint16_t tmp = s.HL();
        s.setHL(s.DE());
        s.setDE(tmp);
        return 4;
    }

    // ── ADD S ────────────────────────────────────────────────────────────────
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87: {
        uint8_t rval = reg_read(s, sss);
        uint16_t res = uint16_t(s.A) + uint16_t(rval);
        update_flags_add(s, res, s.A, rval);
        s.A = uint8_t(res);
        return (sss == 6) ? 7 : 4;
    }

    // ── ADI # ────────────────────────────────────────────────────────────────
    case 0xC6: {
        uint8_t imm = s.next8();
        uint16_t res = uint16_t(s.A) + uint16_t(imm);
        update_flags_add(s, res, s.A, imm);
        s.A = uint8_t(res);
        return 7;
    }

    // ── ADC S ────────────────────────────────────────────────────────────────
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        uint8_t rval = reg_read(s, sss);
        uint8_t cy   = s.flag_cy() ? 1 : 0;
        uint16_t res = uint16_t(s.A) + uint16_t(rval) + cy;
        update_flags_add(s, res, s.A, rval, cy);
        s.A = uint8_t(res);
        return (sss == 6) ? 7 : 4;
    }

    // ── ACI # ────────────────────────────────────────────────────────────────
    case 0xCE: {
        uint8_t imm = s.next8();
        uint8_t cy  = s.flag_cy() ? 1 : 0;
        uint16_t res = uint16_t(s.A) + uint16_t(imm) + cy;
        update_flags_add(s, res, s.A, imm, cy);
        s.A = uint8_t(res);
        return 7;
    }

    // ── SUB S ────────────────────────────────────────────────────────────────
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97: {
        uint8_t rval = reg_read(s, sss);
        uint8_t prev = s.A;
        update_flags_sub(s, s.A, rval);
        s.A = prev - rval;
        return (sss == 6) ? 7 : 4;
    }

    // ── SUI # ────────────────────────────────────────────────────────────────
    case 0xD6: {
        uint8_t imm  = s.next8();
        uint8_t prev = s.A;
        update_flags_sub(s, s.A, imm);
        s.A = prev - imm;
        return 7;
    }

    // ── SBB S ────────────────────────────────────────────────────────────────
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        uint8_t rval = reg_read(s, sss);
        uint8_t cy   = s.flag_cy() ? 1 : 0;
        uint8_t prev = s.A;
        update_flags_sub(s, s.A, rval, cy);
        s.A = prev - rval - cy;
        return (sss == 6) ? 7 : 4;
    }

    // ── SBI # ────────────────────────────────────────────────────────────────
    case 0xDE: {
        uint8_t imm  = s.next8();
        uint8_t cy   = s.flag_cy() ? 1 : 0;
        uint8_t prev = s.A;
        update_flags_sub(s, s.A, imm, cy);
        s.A = prev - imm - cy;
        return 7;
    }

    // ── INR D ────────────────────────────────────────────────────────────────
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
        uint8_t v    = reg_read(s, ddd);
        uint8_t res  = v + 1;
        // INR does NOT affect CY; AC = carry from bit 3 to bit 4
        s.set_ac((v & 0x0F) == 0x0F);
        update_szp(s, res);
        reg_write(s, ddd, res);
        return (ddd == 6) ? 10 : 5;
    }

    // ── DCR D ────────────────────────────────────────────────────────────────
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D: {
        uint8_t v   = reg_read(s, ddd);
        uint8_t res = v - 1;
        // AC set if lower nibble was 0 (borrow from bit 4)
        s.set_ac((v & 0x0F) == 0x00);
        update_szp(s, res);
        reg_write(s, ddd, res);
        return (ddd == 6) ? 10 : 5;
    }

    // ── INX RP ───────────────────────────────────────────────────────────────
    case 0x03: case 0x13: case 0x23: case 0x33:
        rp_write(s, rp, rp_read(s, rp) + 1);
        return 5;

    // ── DCX RP ───────────────────────────────────────────────────────────────
    case 0x0B: case 0x1B: case 0x2B: case 0x3B:
        rp_write(s, rp, rp_read(s, rp) - 1);
        return 5;

    // ── DAD RP ───────────────────────────────────────────────────────────────
    case 0x09: case 0x19: case 0x29: case 0x39: {
        uint32_t res = uint32_t(s.HL()) + uint32_t(rp_read(s, rp));
        s.set_cy(res > 0xFFFF);
        s.setHL(uint16_t(res));
        return 10;
    }

    // ── DAA ──────────────────────────────────────────────────────────────────
    case 0x27: {
        uint8_t  corr = 0;
        bool     new_cy = false;
        uint8_t  lo = s.A & 0x0F;
        // Low nibble correction
        if (s.flag_ac() || lo > 9) {
            corr |= 0x06;
        }
        // High nibble correction
        if (s.flag_cy() || s.A > 0x99) {
            corr  |= 0x60;
            new_cy = true;
        }
        // AC is set if carry out of bit 3 during the adjustment
        s.set_ac(((s.A & 0x0F) + (corr & 0x0F)) > 0x0F);
        s.A += corr;
        update_szp(s, s.A);
        s.set_cy(new_cy);
        return 4;
    }

    // ── ANA S ────────────────────────────────────────────────────────────────
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: {
        // AC = OR of bit 3 of both operands (8080 behavior)
        uint8_t rval = reg_read(s, sss);
        s.set_ac(((s.A | rval) & 0x08) != 0);
        s.A &= rval;
        update_szp(s, s.A);
        s.set_cy(false);
        return (sss == 6) ? 7 : 4;
    }

    // ── ANI # ────────────────────────────────────────────────────────────────
    case 0xE6: {
        uint8_t imm = s.next8();
        s.set_ac(((s.A | imm) & 0x08) != 0);
        s.A &= imm;
        update_szp(s, s.A);
        s.set_cy(false);
        return 7;
    }

    // ── ORA S ────────────────────────────────────────────────────────────────
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        s.A |= reg_read(s, sss);
        update_szp(s, s.A);
        s.set_cy(false);
        s.set_ac(false);
        return (sss == 6) ? 7 : 4;
    }

    // ── ORI # ────────────────────────────────────────────────────────────────
    case 0xF6: {
        uint8_t imm = s.next8();
        s.A |= imm;
        update_szp(s, s.A);
        s.set_cy(false);
        s.set_ac(false);
        return 7;
    }

    // ── XRA S ────────────────────────────────────────────────────────────────
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
        s.A ^= reg_read(s, sss);
        update_szp(s, s.A);
        s.set_cy(false);
        s.set_ac(false);
        return (sss == 6) ? 7 : 4;
    }

    // ── XRI # ────────────────────────────────────────────────────────────────
    case 0xEE: {
        uint8_t imm = s.next8();
        s.A ^= imm;
        update_szp(s, s.A);
        s.set_cy(false);
        s.set_ac(false);
        return 7;
    }

    // ── CMP S ────────────────────────────────────────────────────────────────
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        uint8_t rval = reg_read(s, sss);
        update_flags_sub(s, s.A, rval);
        // A is unchanged
        return (sss == 6) ? 7 : 4;
    }

    // ── CPI # ────────────────────────────────────────────────────────────────
    case 0xFE: {
        uint8_t imm = s.next8();
        update_flags_sub(s, s.A, imm);
        return 7;
    }

    // ── RLC ──────────────────────────────────────────────────────────────────
    case 0x07: {
        uint8_t msb = (s.A >> 7) & 1;
        s.A = (s.A << 1) | msb;
        s.set_cy(msb);
        return 4;
    }

    // ── RRC ──────────────────────────────────────────────────────────────────
    case 0x0F: {
        uint8_t lsb = s.A & 1;
        s.A = (s.A >> 1) | (lsb << 7);
        s.set_cy(lsb);
        return 4;
    }

    // ── RAL ──────────────────────────────────────────────────────────────────
    case 0x17: {
        uint8_t msb = (s.A >> 7) & 1;
        s.A = (s.A << 1) | (s.flag_cy() ? 1 : 0);
        s.set_cy(msb);
        return 4;
    }

    // ── RAR ──────────────────────────────────────────────────────────────────
    case 0x1F: {
        uint8_t lsb = s.A & 1;
        s.A = (s.A >> 1) | (s.flag_cy() ? 0x80 : 0x00);
        s.set_cy(lsb);
        return 4;
    }

    // ── CMA ──────────────────────────────────────────────────────────────────
    case 0x2F:
        s.A = ~s.A;
        return 4;

    // ── CMC ──────────────────────────────────────────────────────────────────
    case 0x3F:
        s.set_cy(!s.flag_cy());
        return 4;

    // ── STC ──────────────────────────────────────────────────────────────────
    case 0x37:
        s.set_cy(true);
        return 4;

    // ── JMP a ────────────────────────────────────────────────────────────────
    case 0xC3:
    case 0xCB:  // undocumented JMP alias
    {
        uint16_t addr = s.next16();
        s.PC = addr;
        return 10;
    }

    // ── Jccc a (11CCC010) ────────────────────────────────────────────────────
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
    case 0xE2: case 0xEA: case 0xF2: case 0xFA: {
        uint16_t addr = s.next16();
        uint8_t  ccc  = (opcode >> 3) & 0x07;
        if (condition(s, ccc)) s.PC = addr;
        return 10;
    }

    // ── CALL a ───────────────────────────────────────────────────────────────
    case 0xCD:
    case 0xDD: case 0xED: case 0xFD: // undocumented CALL aliases
    {
        uint16_t addr = s.next16();
        s.push16(s.PC);
        s.PC = addr;
        return 17;
    }

    // ── Cccc a (11CCC100) ────────────────────────────────────────────────────
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
    case 0xE4: case 0xEC: case 0xF4: case 0xFC: {
        uint16_t addr = s.next16();
        uint8_t  ccc  = (opcode >> 3) & 0x07;
        if (condition(s, ccc)) {
            s.push16(s.PC);
            s.PC = addr;
            return 17;
        }
        return 11;
    }

    // ── RET ──────────────────────────────────────────────────────────────────
    case 0xC9:
    case 0xD9:  // undocumented RET alias
        s.PC = s.pop16();
        return 10;

    // ── Rccc (11CCC000) ──────────────────────────────────────────────────────
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
    case 0xE0: case 0xE8: case 0xF0: case 0xF8: {
        uint8_t ccc = (opcode >> 3) & 0x07;
        if (condition(s, ccc)) {
            s.PC = s.pop16();
            return 11;
        }
        return 5;
    }

    // ── RST n (11NNN111) ─────────────────────────────────────────────────────
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF: {
        s.push16(s.PC);
        s.PC = uint16_t(opcode & 0x38);   // n * 8
        return 11;
    }

    // ── PCHL ─────────────────────────────────────────────────────────────────
    case 0xE9:
        s.PC = s.HL();
        return 5;

    // ── PUSH RP (11RP0101) ───────────────────────────────────────────────────
    case 0xC5: case 0xD5: case 0xE5: case 0xF5:
        s.push16(rp_read_psw(s, rp));
        return 11;

    // ── POP RP (11RP0001) ────────────────────────────────────────────────────
    case 0xC1: case 0xD1: case 0xE1: case 0xF1:
        rp_write_psw(s, rp, s.pop16());
        return 10;

    // ── XTHL ─────────────────────────────────────────────────────────────────
    case 0xE3: {
        uint16_t top = s.read16(s.SP);
        s.write16(s.SP, s.HL());
        s.setHL(top);
        return 18;
    }

    // ── SPHL ─────────────────────────────────────────────────────────────────
    case 0xF9:
        s.SP = s.HL();
        return 5;

    // ── IN p ─────────────────────────────────────────────────────────────────
    case 0xDB: {
        uint8_t port = s.next8();
        if (io.in_handler)
            s.A = io.in_handler(port);
        else
            s.A = 0xFF;  // unimplemented: pull high
        return 10;
    }

    // ── OUT p ────────────────────────────────────────────────────────────────
    case 0xD3: {
        uint8_t port = s.next8();
        if (io.out_handler)
            io.out_handler(port, s.A);
        return 10;
    }

    // ── EI / DI ──────────────────────────────────────────────────────────────
    case 0xFB: s.inte = true;  return 4;
    case 0xF3: s.inte = false; return 4;

    default:
        // Handle MOV D,S block (0x40–0x7F, excluding 0x76 = HLT)
        if (opcode >= 0x40 && opcode <= 0x7F) {
            uint8_t src = reg_read(s, sss);
            reg_write(s, ddd, src);
            bool src_m = (sss == 6);
            bool dst_m = (ddd == 6);
            return (src_m || dst_m) ? 7 : 5;
        }
        // Truly unimplemented opcode — skip it silently in a real emulator
        // you might raise an exception or log a warning here.
        return 4;
    }
}

// ─── LoadBinary ───────────────────────────────────────────────────────────────
void LoadBinary(State8080& state, const char* path, uint16_t offset) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::perror(path);
        throw std::runtime_error(std::string("Cannot open: ") + path);
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);

    if (size < 0 || size > (0x10000 - offset)) {
        std::fclose(f);
        throw std::runtime_error("Binary too large for memory at given offset");
    }
    std::fread(state.mem.data() + offset, 1, static_cast<size_t>(size), f);
    std::fclose(f);
}
