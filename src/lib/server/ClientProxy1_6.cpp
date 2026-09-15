/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2015-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "server/ClientProxy1_6.h"

#include "server/Server.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/StreamChunker.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string& name, barrier::IStream* stream, Server* server,
                               IEventQueue* events) :
    ClientProxy1_5(name, stream, server, events),
    m_events(events)
{
    m_events->adoptHandler(m_events->forClipboard().clipboardSending(),
                                this,
                                new TMethodEventJob<ClientProxy1_6>(this,
                                    &ClientProxy1_6::handleClipboardSendingEvent));
}

ClientProxy1_6::~ClientProxy1_6()
{
}

void
ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
    // ignore if this clipboard is already clean
    if (m_clipboard[id].m_dirty) {
        // this clipboard is now clean
        m_clipboard[id].m_dirty = false;
        Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

        std::string data = m_clipboard[id].m_clipboard.marshall();

        size_t size = data.size();
        LOG((CLOG_DEBUG "sending clipboard %d to \"%s\"", id, getName().c_str()));

        StreamChunker::sendClipboard(data, size, id, 0, m_events, this);
    }
}

void
ClientProxy1_6::handleClipboardSendingEvent(const Event& event, void*)
{
    ClipboardChunk::send(getStream(), event.getData());
}

bool
ClientProxy1_6::recvClipboard()
{
    // parse message
    static std::string dataCached;
    ClipboardID id;
    UInt32 seq;

    int r = ClipboardChunk::assemble(getStream(), dataCached, id, seq);

    if (r == kStart) {
        size_t size = ClipboardChunk::getExpectedSize();
        LOG((CLOG_DEBUG "receiving clipboard %d size=%d", id, size));
    }
    else if (r == kFinish) {
        LOG((CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%d",
                getName().c_str(), id, seq, dataCached.size()));
        // save clipboard
        m_clipboard[id].m_clipboard.unmarshall(dataCached, 0);
        m_clipboard[id].m_sequenceNumber = seq;

        // notify
        ClipboardInfo* info = new ClipboardInfo;
        info->m_id = id;
        info->m_sequenceNumber = seq;
        m_events->addEvent(Event(m_events->forClipboard().clipboardChanged(),
                                 getEventTarget(), info));
    }

    return true;
}

bool
ClientProxy1_6::parseMessage(const UInt8* code)
{
	if (memcmp(code, kMsgDTransferManifest, 4) == 0) { transferManifestReceived(); }
	else if (memcmp(code, kMsgDTransferAccept, 4) == 0) { transferAcceptReceived(); }
	else if (memcmp(code, kMsgDTransferChunk, 4) == 0) { transferChunkReceived(); }
	else if (memcmp(code, kMsgDTransferResume, 4) == 0) { transferResumeReceived(); }
	else if (memcmp(code, kMsgDTransferFinished, 4) == 0) { transferFinishedReceived(); }
	else if (memcmp(code, kMsgDTransferCancel, 4) == 0) { transferCancelReceived(); }
	else { return ClientProxy1_5::parseMessage(code); }
	return true;
}

void ClientProxy1_6::transferManifestSending(std::uint64_t id, const std::string& manifest)
{
	ProtocolUtil::writef(getStream(), kMsgDTransferManifest, id, &manifest);
}
void ClientProxy1_6::transferAcceptSending(std::uint64_t id, bool accepted)
{
	ProtocolUtil::writef(getStream(), kMsgDTransferAccept, id, accepted ? 1 : 0);
}
void ClientProxy1_6::transferChunkSending(std::uint64_t id, UInt32 entry, std::uint64_t offset, const std::string& data)
{
	ProtocolUtil::writef(getStream(), kMsgDTransferChunk, id, entry, offset, &data);
}
void ClientProxy1_6::transferResumeSending(std::uint64_t id, UInt32 entry, std::uint64_t offset)
{
	ProtocolUtil::writef(getStream(), kMsgDTransferResume, id, entry, offset);
}
void ClientProxy1_6::transferFinishedSending(std::uint64_t id, bool success)
{
	ProtocolUtil::writef(getStream(), kMsgDTransferFinished, id, success ? 1 : 0);
}
void ClientProxy1_6::transferCancelSending(std::uint64_t id)
{
	ProtocolUtil::writef(getStream(), kMsgDTransferCancel, id);
}

void ClientProxy1_6::transferManifestReceived()
{
	std::uint64_t id; std::string manifest;
	if (ProtocolUtil::readf(getStream(), kMsgDTransferManifest + 4, &id, &manifest)) m_server->transferManifestReceived(this, id, manifest);
}
void ClientProxy1_6::transferAcceptReceived()
{
	std::uint64_t id; UInt8 accepted;
	if (ProtocolUtil::readf(getStream(), kMsgDTransferAccept + 4, &id, &accepted)) m_server->transferAcceptReceived(this, id, accepted != 0);
}
void ClientProxy1_6::transferChunkReceived()
{
	std::uint64_t id, offset; UInt32 entry; std::string data;
	if (ProtocolUtil::readf(getStream(), kMsgDTransferChunk + 4, &id, &entry, &offset, &data)) m_server->transferChunkReceived(this, id, entry, offset, data);
}
void ClientProxy1_6::transferResumeReceived()
{
	std::uint64_t id, offset; UInt32 entry;
	if (ProtocolUtil::readf(getStream(), kMsgDTransferResume + 4, &id, &entry, &offset)) m_server->transferResumeReceived(this, id, entry, offset);
}
void ClientProxy1_6::transferFinishedReceived()
{
	std::uint64_t id; UInt8 success;
	if (ProtocolUtil::readf(getStream(), kMsgDTransferFinished + 4, &id, &success)) m_server->transferFinishedReceived(this, id, success != 0);
}
void ClientProxy1_6::transferCancelReceived()
{
	std::uint64_t id;
	if (ProtocolUtil::readf(getStream(), kMsgDTransferCancel + 4, &id)) m_server->transferCancelReceived(this, id);
}
