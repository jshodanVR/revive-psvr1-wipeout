// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace sha256 {

// Minimal self-contained SHA-256 (FIPS 180-4) matching the style of common/sha1.h. Used only
// for deterministic content verification of patch XML files; not for authentication.
class SHA256 {
public:
    using digest_t = std::array<uint8_t, 32>;

    SHA256() {
        reset();
    }

    SHA256& reset() {
        state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        bit_count_ = 0;
        block_len_ = 0;
        return *this;
    }

    SHA256& update(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        bit_count_ += static_cast<uint64_t>(size) * 8u;
        while (size > 0) {
            const size_t take = std::min(size, sizeof(block_) - block_len_);
            std::memcpy(block_ + block_len_, bytes, take);
            block_len_ += take;
            bytes += take;
            size -= take;
            if (block_len_ == sizeof(block_)) {
                transform(block_);
                block_len_ = 0;
            }
        }
        return *this;
    }

    SHA256& update(std::string_view data) {
        return update(data.data(), data.size());
    }

    digest_t final() {
        digest_t digest;
        const uint64_t bit_count = bit_count_;

        // Padding: 0x80 followed by zeroes, then a 64-bit big-endian bit length.
        block_[block_len_++] = 0x80;
        if (block_len_ > 56) {
            while (block_len_ < sizeof(block_)) {
                block_[block_len_++] = 0;
            }
            transform(block_);
            block_len_ = 0;
        }
        while (block_len_ < 56) {
            block_[block_len_++] = 0;
        }
        for (int i = 7; i >= 0; --i) {
            block_[block_len_++] = static_cast<uint8_t>(bit_count >> (i * 8));
        }
        transform(block_);

        for (size_t i = 0; i < 8; ++i) {
            const uint32_t v = state_[i];
            digest[i * 4 + 0] = static_cast<uint8_t>(v >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(v >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(v >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(v);
        }
        reset();
        return digest;
    }

    static std::string Hex(std::string_view data) {
        SHA256 hasher;
        hasher.update(data);
        return Hex(hasher.final());
    }

    static std::string Hex(const digest_t& digest) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(digest.size() * 2);
        for (uint8_t byte : digest) {
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
        return out;
    }

private:
    static constexpr std::array<uint32_t, 64> kRoundConstants = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    static constexpr uint32_t RightRotate(uint32_t value, uint32_t count) {
        return (value >> count) | (value << (32 - count));
    }

    void transform(const uint8_t* block) {
        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 =
                RightRotate(w[i - 15], 7) ^ RightRotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 =
                RightRotate(w[i - 2], 17) ^ RightRotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = RightRotate(e, 6) ^ RightRotate(e, 11) ^ RightRotate(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + S1 + ch + kRoundConstants[i] + w[i];
            const uint32_t S0 = RightRotate(a, 2) ^ RightRotate(a, 13) ^ RightRotate(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{};
    uint64_t bit_count_{0};
    size_t block_len_{0};
    uint8_t block_[64]{};
};

} // namespace sha256
