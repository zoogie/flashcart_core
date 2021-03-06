#include <cstdint>

#include "platform.h"

using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::size_t;

#define BSWAP32(val) ((((val >> 24) & 0xFF)) | (((val >> 16) & 0xFF) << 8) | (((val >> 8) & 0xFF) << 16) | ((val & 0xFF) << 24))
#define BSWAP64(val) ((((val >> 56) & 0xFF)) | (((val >> 48) & 0xFF) << 8) | (((val >> 40) & 0xFF) << 16) | (((val >> 32) & 0xFF) << 24) | \
    (((val >> 24) & 0xFF) << 32) | (((val >> 16) & 0xFF) << 40) | (((val >> 8) & 0xFF) << 48) | (((val) & 0xFF) << 56))

#define ROMCNT_SEC_LARGE        (1u << 28)              // Use "other" secure area mode, which tranfers blocks of 0x1000 bytes at a time
#define ROMCNT_CLK_SLOW         (1u << 27)              // Transfer clock rate (0 = 6.7MHz, 1 = 4.2MHz)
#define ROMCNT_SEC_CMD          (1u << 22)              // The command transfer will be hardware encrypted (KEY2)
#define ROMCNT_DELAY2(n)        (((n) & 0x3Fu) << 16)   // Transfer delay length part 2
#define ROMCNT_DELAY2_MASK      (ROMCNT_DELAY2(0x3F))
#define ROMCNT_SEC_EN           (1u << 14)              // Security enable
#define ROMCNT_SEC_DAT          (1u << 13)              // The data transfer will be hardware encrypted (KEY2)
#define ROMCNT_DELAY1(n)        ((n) & 0x1FFFu)         // Transfer delay length part 1
#define ROMCNT_DELAY1_MASK      (ROMCNT_DELAY1(0x1FFF))

#define CMD_RAW_DUMMY           0x9Full
#define CMD_RAW_HEADER_READ     0x00ull
#define CMD_RAW_CHIPID          0x90ull
#define CMD_RAW_ACTIVATE_KEY1   0x3Cull

#define CMD_KEY1_INIT_KEY2      0x4ull
#define CMD_KEY1_CHIPID         0x1ull
#define CMD_KEY1_SECURE_READ    0x2ull
#define CMD_KEY1_ACTIVATE_KEY2  0xAull

#define CMD_KEY2_DATA_READ      0xB7ull
#define CMD_KEY2_CHIPID         0xB8ull

namespace flashcart_core {
using ntrcard::BlowfishKey;
using ntrcard::BLOWFISH_PS_N;
using ntrcard::BLOWFISH_P_N;
using ntrcard::state;

namespace {
void blowfish_encrypt(const uint32_t (&ps)[BLOWFISH_PS_N], uint32_t lr[2]) {
    uint32_t x = lr[1];
    uint32_t y = lr[0];

    for(int i = 0; i < 0x10; ++i) {
        uint32_t z = ps[i] ^ x;
        x = ps[0x012 + ((z >> 24) & 0xFF)];
        x = ps[0x112 + ((z >> 16) & 0xFF)] + x;
        x = ps[0x212 + ((z >> 8) & 0xFF)] ^ x;
        x = ps[0x312 + ((z >> 0) & 0xFF)] + x;
        x = y ^ x;
        y = z;
    }

    lr[0] = x ^ ps[0x10];
    lr[1] = y ^ ps[0x11];
}

__attribute__((unused)) void blowfish_decrypt(const uint32_t (&ps)[BLOWFISH_PS_N], uint32_t lr[2]) {
    uint32_t x = lr[1];
    uint32_t y = lr[0];

    for(size_t i = 0x11; i > 1; --i) {
        uint32_t z = ps[i] ^ x;
        x = ps[0x012 + ((z >> 24) & 0xFF)];
        x = ps[0x112 + ((z >> 16) & 0xFF)] + x;
        x = ps[0x212 + ((z >> 8) & 0xFF)] ^ x;
        x = ps[0x312 + ((z >> 0) & 0xFF)] + x;
        x = y ^ x;
        y = z;
    }

    lr[0] = x ^ ps[1];
    lr[1] = y ^ ps[0];
}

void blowfish_apply_key(uint32_t (&ps)[BLOWFISH_PS_N], uint32_t key[3]) {
    uint32_t scratch[2] = {0};

    blowfish_encrypt(ps, key + 1);
    blowfish_encrypt(ps, key);

    for(size_t i = 0; i < BLOWFISH_P_N; ++i) {
        ps[i] = ps[i] ^ BSWAP32(key[i%2]);
    }

    for(size_t i = 0; i < BLOWFISH_PS_N; i += 2) {
        blowfish_encrypt(ps, scratch);
        ps[i] = scratch[1];
        ps[i + 1] = scratch[0];
    }
}

void init_blowfish(BlowfishKey key) {
    if (key != BlowfishKey::NTR) {
        platform::initBlowfishPS(state.key1_ps, key);
    } else {
        state.key1_key[0] = state.game_code;
        state.key1_key[1] = state.game_code >> 1;
        state.key1_key[2] = state.game_code << 1;

        platform::initBlowfishPS(state.key1_ps, BlowfishKey::NTR);

        blowfish_apply_key(state.key1_ps, state.key1_key);
        blowfish_apply_key(state.key1_ps, state.key1_key);
    }
}

void read_header() {
    uint8_t hdr[0x1000];
    ntrcard::sendCommand(CMD_RAW_HEADER_READ, 0x1000, hdr, ROMCNT_CLK_SLOW | ROMCNT_DELAY2(0x18));

    state.game_code = *reinterpret_cast<uint32_t *>(hdr + 0xC);
    state.hdr_key1_romcnt = state.key1_romcnt = *reinterpret_cast<uint32_t *>(hdr + 0x64);
    state.hdr_key2_romcnt = state.key2_romcnt = *reinterpret_cast<uint32_t *>(hdr + 0x60);
    state.key2_seed = *reinterpret_cast<uint8_t *>(hdr + 0x13);
}

void key1_cmdf(const uint8_t cmdarg, const uint32_t size, uint8_t *const dest, const uint16_t arg, const uint32_t ij, const uint32_t flags) {
    // C = cmd, A = arg
    // KK KK JK JJ II AI AA CA
    const uint32_t k = state.key1_k++;
    uint64_t cmd = ((cmdarg & 0xF) << 4) |
        ((arg & 0xF000ull) >> 12 /* << 0 - 12 */) | ((arg & 0xFF0ull) << 4 /* 8 - 4 */) | ((arg & 0xFull) << 20 /* 20 - 0 */) |
        ((ij & 0xF00000ull) >> 4 /* << 16 - 20 */) | ((ij & 0xFF000ull) << 12 /* 24 - 12 */) | ((ij & 0xFF0ull) << 28 /* 32 - 4 */) |
        ((ij & 0xFull) << 44 /* 44 - 0 */) | ((k & 0xF0000ull) << 24 /* 40 - 16 */) | ((k & 0xFF00ull) << 40 /* 48 - 8 */) |
        ((k & 0xFFull) << 56 /* 56 - 0 */);
    cmd = BSWAP64(cmd);
    platform::logMessage(LOG_DEBUG, "Sending KEY1 cmd: %016llX (plaintext)", cmd);
    blowfish_encrypt(state.key1_ps, reinterpret_cast<uint32_t *>(&cmd));
    ntrcard::sendCommand(BSWAP64(cmd), size, dest, flags);
}

void key1_cmd(const uint8_t cmdarg, const uint32_t size, uint8_t *const dest) {
    key1_cmdf(cmdarg, size, dest, state.key1_l, state.key1_ij, state.key1_romcnt);
}

void seed_key2_registers(void) {
    const uint8_t seed_bytes[8] = {0xE8, 0x4D, 0x5A, 0xB1, 0x17, 0x8F, 0x99, 0xD5};
    state.key2_x = seed_bytes[state.key2_seed & 7] + (static_cast<uint64_t>(state.key2_mn) << 15) + 0x6000;
    state.key2_y = 0x5c879b9b05ull;
    platform::logMessage(LOG_DEBUG, "Seed KEY2: %llX %llX", state.key2_x, state.key2_y);
    if (platform::HAS_HW_KEY2) {
        platform::initKey2Seed(state.key2_x, state.key2_y);
    }
}
}

namespace ntrcard {
static_assert(sizeof(OpFlags) == sizeof(std::uint32_t), "OpFlags is too big!");
static_assert(OpFlags(0xA7586000).key2_command(), "OpFlags parsing wrong");
static_assert(OpFlags(0xA7586000).key2_response(), "OpFlags parsing wrong");
static_assert(!OpFlags(0xA7586000).slow_clock(), "OpFlags parsing wrong");
static_assert(!OpFlags(0xA7586000).large_secure_area_read(), "OpFlags parsing wrong");
static_assert(OpFlags(0xA7586123).pre_delay() == 0x123, "OpFlags parsing wrong");
static_assert(OpFlags(0xA7586000).post_delay() == 0x18, "OpFlags parsing wrong");
static_assert(OpFlags(0)
                .key2_command(true).key2_response(true).slow_clock(true)
                .large_secure_area_read(true)
                .pre_delay(0x8F8).post_delay(0x18) == 0x185868F8,
                "OpFlags construction wrong");

State state;

bool sendCommand(const uint8_t *cmdbuf, uint16_t response_len, uint8_t *resp, OpFlags flags) {
    if (state.status == Status::KEY2) {
        flags = flags.key2_command(true).key2_response(true);
    }
    return platform::sendCommand(cmdbuf, response_len, resp, flags);
}

bool sendCommand(const uint64_t cmd, uint16_t response_len, uint8_t *resp, OpFlags flags) {
    return sendCommand(reinterpret_cast<const uint8_t *>(&cmd), response_len, resp, flags);
}

bool init() {
    if (platform::CAN_RESET) {
        uint32_t reset_result = platform::resetCard();
        if (reset_result) {
            platform::logMessage(LOG_ERR, "platform::resetCard failed: %d", reset_result);
            return false;
        }
        state.status = Status::RAW;
    } else if (state.status > Status::RAW) {
        platform::logMessage(LOG_ERR,
            "Trying to re-init after encryption is on on !CAN_RESET platform"
            " (status = %d)",
            static_cast<uint32_t>(state.status));
        return false;
    }

    sendCommand(CMD_RAW_DUMMY, 0x2000, nullptr, ROMCNT_CLK_SLOW | ROMCNT_DELAY2(0x18));
    platform::ioDelay(0x40000);
    sendCommand(CMD_RAW_CHIPID, 4, reinterpret_cast<uint8_t *>(&state.chipid), ROMCNT_CLK_SLOW | ROMCNT_DELAY2(0x18));
    read_header();
    platform::logMessage(LOG_DEBUG, "Cart init; state = { chipid = 0x%08X, game_code = 0x%08X, hdr_key1_romcnt = 0x%08X, hdr_key2_romcnt = 0x%08X, key2_seed = 0x%X }",
        state.chipid, state.game_code, state.hdr_key1_romcnt, state.hdr_key2_romcnt, state.key2_seed);
    return true;
}

bool initKey1(BlowfishKey key) {
    if (!platform::HAS_HW_KEY2) {
        platform::logMessage(LOG_ERR, "Key1 fail due to no Key2 support");
        return false; // TODO impl SW KEY2
    }

    if (state.status != Status::RAW) {
        platform::logMessage(LOG_ERR,
            "Trying to init KEY1 from not RAW (status = %d)",
            static_cast<uint32_t>(state.status));
        return false;
    }

    state.key2_mn = 0xC99ACE;
    state.key1_ij = 0x11A473;
    state.key1_k = 0x39D46;
    state.key1_l = 0;
    init_blowfish(key);

    // 00 KK KK 0K JJ IJ II 3C
    sendCommand(CMD_RAW_ACTIVATE_KEY1 |
        ((state.key1_ij & 0xFF0000ull) >> 8) | ((state.key1_ij & 0xFF00ull) << 8) | ((state.key1_ij & 0xFFull) << 24) |
        ((state.key1_k & 0xF0000ull) << 16) | ((state.key1_k & 0xFF00ull) << 32) | ((state.key1_k & 0xFFull) << 48),
        0, 0, state.key2_romcnt & (ROMCNT_CLK_SLOW | ROMCNT_DELAY2_MASK | ROMCNT_DELAY1_MASK));

    state.key1_romcnt = (state.key2_romcnt & ROMCNT_CLK_SLOW) |
        ((state.hdr_key1_romcnt & (ROMCNT_CLK_SLOW | ROMCNT_DELAY1_MASK)) +
        ((state.hdr_key1_romcnt & ROMCNT_DELAY2_MASK) >> 16)) | ROMCNT_SEC_LARGE;
    key1_cmdf(CMD_KEY1_INIT_KEY2, 0, 0, state.key1_l, state.key2_mn, state.key1_romcnt);

    seed_key2_registers();
    state.key1_romcnt |= ROMCNT_SEC_EN | ROMCNT_SEC_DAT;

    key1_cmd(CMD_KEY1_CHIPID, 4, reinterpret_cast<uint8_t *>(&state.key1_chipid));
    if (state.key1_chipid != state.chipid) {
        platform::logMessage(LOG_ERR, "Key1 fail: mismatching chipid: (raw) %X != (key1) %X", state.chipid, state.key1_chipid);
        state.status = Status::UNKNOWN;
        return false;
    }

    state.status = Status::KEY1;
    return true;
}

bool initKey2() {
    if (!platform::HAS_HW_KEY2) {
        platform::logMessage(LOG_ERR, "Key2 fail due to no Key2 support");
        return false; // TODO impl SW KEY2
    }

    if (state.status != Status::KEY1) {
        platform::logMessage(LOG_ERR,
            "Trying to init KEY2 from not KEY1 (status = %d)",
            static_cast<uint32_t>(state.status));
        return false;
    }

    key1_cmd(CMD_KEY1_ACTIVATE_KEY2, 0, 0);
    state.key2_romcnt = state.hdr_key2_romcnt &
        (ROMCNT_CLK_SLOW | ROMCNT_SEC_CMD | ROMCNT_DELAY2_MASK |
        ROMCNT_SEC_EN | ROMCNT_SEC_DAT | ROMCNT_DELAY1_MASK);

    ntrcard::sendCommand(CMD_KEY2_CHIPID, 4, reinterpret_cast<uint8_t *>(&state.key2_chipid), state.key2_romcnt);
    if (state.key2_chipid != state.chipid) {
        platform::logMessage(LOG_ERR, "Key2 fail: mismatching chipid: (raw) %X != (key2) %X", state.chipid, state.key2_chipid);
        state.status = Status::UNKNOWN;
        return false;
    }

    state.status = Status::KEY2;
    return true;
}
}

namespace {
class init {
public:
    init() {
        state.status = platform::INITIAL_ENCRYPTION;
    }
};

init _;
}
}
