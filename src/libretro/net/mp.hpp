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
#pragma once
#include <cstdint>
#include <queue>
#include <optional>
#include <vector>
#include <libretro.h>

namespace MelonDsDs {
// timestamp, aid, and packet type, respectively.
constexpr size_t HeaderSize = sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint8_t);

class Packet {
public:
    enum Type {
        Reply, Cmd, Ack, Connect, Disconnect, Other
    };

    static Packet parsePk(const void *buf, uint64_t len, uint16_t sender_id);
    explicit Packet(const void *data, uint64_t len, uint64_t timestamp, uint8_t aid, Packet::Type type, uint16_t sender_id = RETRO_NETPACKET_BROADCAST);

    [[nodiscard]] uint64_t Timestamp() const noexcept {
        return _timestamp;
    };
    [[nodiscard]] uint8_t Aid() const noexcept {
        return _aid;
    };
    [[nodiscard]] Packet::Type PacketType() const noexcept {
        return _type;
    }
    [[nodiscard]] uint16_t SenderId() const noexcept {
        return _senderId;
    }
    [[nodiscard]] const void *Data() const noexcept {
        return _data.data();
    };
    [[nodiscard]] uint64_t Length() const noexcept {
        return _data.size();
    };

    std::vector<uint8_t> ToBuf() const;
private:
    uint64_t _timestamp;
    uint8_t _aid;
    Packet::Type _type;
    uint16_t _senderId;
    std::vector<uint8_t> _data;
};

class MpState {
public:
    void Started(uint16_t client_id, retro_netpacket_send_t sendFn, retro_netpacket_poll_receive_t pollFn) noexcept;
    void Stopped() noexcept;
    void Begin() noexcept;
    void End() noexcept;
    bool Connected(uint16_t client_id) noexcept;
    void Disconnected(uint16_t client_id) noexcept;
    void PacketReceived(const void *buf, size_t len, uint16_t client_id) noexcept;
    bool IsReady() const noexcept;
    void SendPacket(const Packet &p) noexcept;
    std::optional<Packet> NextPacket() noexcept;
    std::optional<Packet> NextPacketBlock() noexcept;
    std::optional<Packet> NextPacketOfTypeBlock(Packet::Type type) noexcept;
    [[nodiscard]] uint16_t ClientId() const noexcept;
    [[nodiscard]] uint16_t ConnectedBitmask() const noexcept;
private:
    static uint16_t PeerMask(uint16_t client_id) noexcept;

    std::optional<Packet> PopPacketOfType(Packet::Type type) noexcept;

    bool _warnedHighLatency = false;
    int _timeoutCount = 0;
    retro_netpacket_send_t _sendFn = nullptr;
    retro_netpacket_poll_receive_t _pollFn = nullptr;
    uint16_t _clientId = RETRO_NETPACKET_BROADCAST;
    uint16_t _connectedBitmask = 0;
    std::optional<uint16_t> _hostId;
    std::queue<Packet> receivedPackets;
};
}
