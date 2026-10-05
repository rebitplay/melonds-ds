/*
    Copyright 2024 Bernardo Gomes Negri

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/
#include "mp.hpp"
#include "environment.hpp"
#include <chrono>
#include <thread>
#include <libretro.h>
#include <retro_assert.h>
#include <retro_endianness.h>
using namespace MelonDsDs;

// How many successive timeouts before
// the player gets notified they are not supposed to use a VPN.
constexpr int SUCCESSIVE_TIMEOUTS_WARNING = 6;
constexpr long RECV_TIMEOUT_MS = 25;

uint64_t swapToNetwork(uint64_t n) {
    return swap_if_little64(n);
}

Packet Packet::parsePk(const void *buf, uint64_t len, uint16_t sender_id) {
    // Necessary because arithmetic on void* is forbidden
    const char *indexableBuf = (const char *)buf;
    const char *data = indexableBuf + HeaderSize;
    retro_assert(len >= HeaderSize);
    size_t dataLen = len - HeaderSize;
    uint64_t timestamp = swapToNetwork(*(const uint64_t*)(indexableBuf));
    uint8_t aid = *(const uint8_t*)(indexableBuf + 8);
    uint8_t type = *(const uint8_t*)(indexableBuf + 9);
    // The libretro netpacket bridge historically uses:
    // type 2: host command, type 1: client reply, type 3: host ACK,
    // type 4/5: MP-active peer connect/disconnect, type 0: any other MP frame.
    retro_assert(type == 5 || type == 4 || type == 3 || type == 2 || type == 1 || type == 0);
    Packet::Type pkType;
    switch (type) {
        case 0:
            pkType = Other;
            break;
        case 1:
            pkType = Reply;
            break;
        case 2:
            pkType = Cmd;
            break;
        case 3:
            pkType = Ack;
            break;
        case 4:
            pkType = Connect;
            break;
        case 5:
            pkType = Disconnect;
            break;
    }
    return Packet(data, dataLen, timestamp, aid, pkType, sender_id);
}

Packet::Packet(const void *data, uint64_t len, uint64_t timestamp, uint8_t aid, Packet::Type type, uint16_t sender_id) :
    _data(),
    _timestamp(timestamp),
    _aid(aid),
    _type(type),
    _senderId(sender_id) {
    if (data != nullptr && len > 0) {
        const auto *bytes = static_cast<const uint8_t*>(data);
        _data.assign(bytes, bytes + len);
    }
}

std::vector<uint8_t> Packet::ToBuf() const {
    std::vector<uint8_t> ret;
    ret.reserve(HeaderSize + Length());
    uint64_t netTimestamp = swapToNetwork(_timestamp);
    ret.insert(ret.end(), (const char *)&netTimestamp, ((const char *)&netTimestamp) + sizeof(uint64_t));
    ret.push_back(_aid);
    uint8_t numericalType = 0;
    switch(_type) {
        case Other:
            numericalType = 0;
            break;
        case Reply:
            numericalType = 1;
            break;
        case Cmd:
            numericalType = 2;
            break;
        case Ack:
            numericalType = 3;
            break;
        case Connect:
            numericalType = 4;
            break;
        case Disconnect:
            numericalType = 5;
            break;
    }
    ret.push_back(numericalType);
    ret.insert(ret.end(), _data.begin(), _data.end());
    return ret;
}

bool MpState::IsReady() const noexcept {
    return _sendFn != nullptr && _pollFn != nullptr;
}

uint16_t MpState::PeerMask(uint16_t client_id) noexcept {
    if (client_id >= 16) {
        return 0;
    }
    return 1 << client_id;
}

void MpState::Started(uint16_t client_id, retro_netpacket_send_t sendFn, retro_netpacket_poll_receive_t pollFn) noexcept {
    if (sendFn != nullptr) {
        retro::set_warn_message("LAN Multiplayer will NOT work using VPNs or tunnels such as Hamachi!");
    }
    _clientId = client_id;
    _sendFn = sendFn;
    _pollFn = pollFn;
    _connectedBitmask = 0;
}

void MpState::Stopped() noexcept {
    _sendFn = nullptr;
    _pollFn = nullptr;
    _clientId = RETRO_NETPACKET_BROADCAST;
    _connectedBitmask = 0;
    _hostId = std::nullopt;
    while (!receivedPackets.empty()) {
        receivedPackets.pop();
    }
}

void MpState::Begin() noexcept {
    _connectedBitmask |= PeerMask(_clientId);
    if (IsReady()) {
        SendPacket(Packet(nullptr, 0, 0, 0, Packet::Type::Connect));
    }
}

void MpState::End() noexcept {
    if (IsReady()) {
        SendPacket(Packet(nullptr, 0, 0, 0, Packet::Type::Disconnect));
    }
    _connectedBitmask &= ~PeerMask(_clientId);
}

bool MpState::Connected(uint16_t client_id) noexcept {
    (void)client_id;
    return true;
}

void MpState::Disconnected(uint16_t client_id) noexcept {
    _connectedBitmask &= ~PeerMask(client_id);
    if (_hostId == client_id) {
        _hostId = std::nullopt;
    }
}

void MpState::PacketReceived(const void *buf, size_t len, uint16_t client_id) noexcept {
    retro_assert(IsReady());
    Packet p = Packet::parsePk(buf, len, client_id);
    if(p.PacketType() == Packet::Type::Connect) {
        _connectedBitmask |= PeerMask(client_id);
        return;
    }
    if(p.PacketType() == Packet::Type::Disconnect) {
        Disconnected(client_id);
        return;
    }
    if(p.PacketType() == Packet::Type::Cmd) {
        _hostId = client_id;
        //retro::debug("Host client id is {}", client_id);
    }
    receivedPackets.push(std::move(p));
}

std::optional<Packet> MpState::NextPacket() noexcept {
    retro_assert(IsReady());
    if(receivedPackets.empty()) {
        _sendFn(RETRO_NETPACKET_FLUSH_HINT, NULL, 0, RETRO_NETPACKET_BROADCAST);
        _pollFn();
    }
    if(receivedPackets.empty()) {
        return std::nullopt;
    } else {
        _timeoutCount = 0;
        Packet p = receivedPackets.front();
        receivedPackets.pop();
        return p;
    }
}

std::optional<Packet> MpState::NextPacketBlock() noexcept {
    retro_assert(IsReady());
    if (receivedPackets.empty()) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(RECV_TIMEOUT_MS);
        while(std::chrono::steady_clock::now() < deadline) {
            _sendFn(RETRO_NETPACKET_FLUSH_HINT, NULL, 0, RETRO_NETPACKET_BROADCAST);
            _pollFn();
            if(!receivedPackets.empty()) {
                return NextPacket();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        return NextPacket();
    }
    _timeoutCount++;
    if (_timeoutCount >= SUCCESSIVE_TIMEOUTS_WARNING && !_warnedHighLatency) {
        retro::set_warn_message("LAN Multiplayer will NOT work using VPNs or tunnels such as Hamachi!");
        _warnedHighLatency = true;
    }
    retro::debug("Timeout while waiting for packet");
    return std::nullopt;
}

std::optional<Packet> MpState::PopPacketOfType(Packet::Type type) noexcept {
    retro_assert(IsReady());

    std::optional<Packet> ret = std::nullopt;
    std::queue<Packet> deferred;

    while (!receivedPackets.empty()) {
        Packet p = receivedPackets.front();
        receivedPackets.pop();

        if (!ret.has_value() && p.PacketType() == type) {
            ret = std::move(p);
        } else {
            deferred.push(std::move(p));
        }
    }

    receivedPackets = std::move(deferred);

    if (ret.has_value()) {
        _timeoutCount = 0;
    }

    return ret;
}

std::optional<Packet> MpState::NextPacketOfTypeBlock(Packet::Type type) noexcept {
    retro_assert(IsReady());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(RECV_TIMEOUT_MS);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::optional<Packet> p = PopPacketOfType(type); p.has_value()) {
            return p;
        }

        _sendFn(RETRO_NETPACKET_FLUSH_HINT, NULL, 0, RETRO_NETPACKET_BROADCAST);
        _pollFn();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    _timeoutCount++;
    if (_timeoutCount >= SUCCESSIVE_TIMEOUTS_WARNING && !_warnedHighLatency) {
        retro::set_warn_message("LAN Multiplayer will NOT work using VPNs or tunnels such as Hamachi!");
        _warnedHighLatency = true;
    }
    retro::debug("Timeout while waiting for packet type {}", static_cast<int>(type));
    return std::nullopt;
}

void MpState::SendPacket(const Packet &p) noexcept {
    retro_assert(IsReady());
    uint16_t dest = RETRO_NETPACKET_BROADCAST;
    if(p.PacketType() == Packet::Type::Cmd) {
        _hostId = std::nullopt;
    }
    if(p.PacketType() == Packet::Type::Reply && _hostId.has_value()) {
        dest = _hostId.value();
    }
    int flags = RETRO_NETPACKET_UNSEQUENCED | RETRO_NETPACKET_UNRELIABLE | RETRO_NETPACKET_FLUSH_HINT;
    if(p.PacketType() == Packet::Type::Connect || p.PacketType() == Packet::Type::Disconnect) {
        flags = RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT;
    }
    auto buf = p.ToBuf();
    _sendFn(flags, buf.data(), buf.size(), dest);
}

uint16_t MpState::ClientId() const noexcept {
    return _clientId;
}

uint16_t MpState::ConnectedBitmask() const noexcept {
    return _connectedBitmask;
}
