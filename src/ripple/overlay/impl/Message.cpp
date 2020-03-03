//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/basics/safe_cast.h>
#include <ripple/overlay/Message.h>
#include <ripple/overlay/impl/TrafficCount.h>
#include <ripple/overlay/Compression.h>
#include <ripple/app/main/Application.h>
#include <cstdint>

namespace ripple {

Message::Message (::google::protobuf::Message const& message, int type, bool compression_enabled)
    : mCategory(TrafficCount::categorize(message, type, false))
{

#if defined(GOOGLE_PROTOBUF_VERSION) && (GOOGLE_PROTOBUF_VERSION >= 3011000)
    auto const messageBytes = message.ByteSizeLong ();
#else
    unsigned const messageBytes = message.ByteSize ();
#endif

    assert (messageBytes != 0);

    /** Number of bytes in a message header. */
    std::size_t constexpr headerBytes = 6;

    mBuffer.resize (headerBytes + messageBytes);

    auto set_header = [](uint8_t *ptr, decltype(messageBytes) messageBytes, int type, bool compressed=false,
            uint8_t compr_algorithm=ripple::compression::Algorithm::LZ4) {
        uint8_t compression = (compressed?0xF0:0) & (0x80 | (compr_algorithm << 4));
        *ptr++ = static_cast<std::uint8_t>(((messageBytes >> 24) | compression) & 0xFF);
        *ptr++ = static_cast<std::uint8_t>((messageBytes >> 16) & 0xFF);
        *ptr++ = static_cast<std::uint8_t>((messageBytes >> 8) & 0xFF);
        *ptr++ = static_cast<std::uint8_t>(messageBytes & 0xFF);

        *ptr++ = static_cast<std::uint8_t>((type >> 8) & 0xFF);
        *ptr = static_cast<std::uint8_t> (type & 0xFF);
    };

    set_header(mBuffer.data(), messageBytes, type);

    if (messageBytes != 0)
        message.SerializeToArray(mBuffer.data() + headerBytes, messageBytes);

    bool compressible = compression_enabled &&
            (type == protocol::mtMANIFESTS || type == protocol::mtENDPOINTS ||
             type == protocol::mtTRANSACTION || type == protocol::mtGET_LEDGER || type == protocol::mtLEDGER_DATA ||
             type == protocol::mtGET_OBJECTS || type == protocol::mtVALIDATORLIST) &&
            messageBytes > 70;

    int comprSize = 0;
    if (compressible)
    {
        auto *payload = static_cast<void const*>(mBuffer.data() + headerBytes);

        auto res = ripple::compression::compress(
                payload,
                messageBytes,
                [this, headerBytes](std::size_t in_size) {
                    mBufferCompressed.resize(in_size + headerBytes);
                    return (mBufferCompressed.data() + headerBytes);
        });

        decltype(messageBytes) compressedSize = std::get<1>(res);
        double ratio = 1.0 - (double)compressedSize / (double)messageBytes;

        // TODO should we have min acceptable compression ratio?
        if (ratio > 0.0)
        {
            // set the buffer to the header size + compressed size
            mBufferCompressed.resize(headerBytes + compressedSize);
            comprSize = compressedSize;
            set_header(mBufferCompressed.data(), compressedSize, type, true);
        }
        else
            mBufferCompressed.resize(0);
    }
}

}
