/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#include "barrier/StreamChunker.h"

#include "mt/Lock.h"
#include "mt/Mutex.h"
#include "barrier/FileChunk.h"
#include "barrier/TransferManifest.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/protocol_types.h"
#include "base/EventTypes.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/EventTypes.h"
#include "base/Log.h"
#include "base/Stopwatch.h"
#include "base/String.h"

#include <fstream>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>

using namespace std;

static const size_t g_chunkSize = 32 * 1024; //32kb
static std::uint64_t g_nextTransferId = 1;
namespace {
struct TransferAcceptance {
	bool ready;
	bool accepted;
};
std::mutex g_transferMutex;
std::condition_variable g_transferCondition;
std::map<std::uint64_t, TransferAcceptance> g_transferAcceptances;
}

bool StreamChunker::s_isChunkingFile = false;
bool StreamChunker::s_interruptFile = false;
Mutex* StreamChunker::s_interruptMutex = NULL;

void
StreamChunker::sendFile(const char* filename,
                IEventQueue* events,
                void* eventTarget)
{
    s_isChunkingFile = true;

    std::fstream file(filename, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
        throw runtime_error("failed to open file");
    }

    // check file size
    file.seekg (0, std::ios::end);
    size_t size = (size_t)file.tellg();

    // send first message (file size)
    String fileSize = barrier::string::sizeTypeToString(size);
    FileChunk* sizeMessage = FileChunk::start(fileSize);

    events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, sizeMessage));

    // send chunk messages with a fixed chunk size
    size_t sentLength = 0;
    size_t chunkSize = g_chunkSize;
    file.seekg (0, std::ios::beg);

    while (true) {
        if (s_interruptFile) {
            s_interruptFile = false;
            LOG((CLOG_DEBUG "file transmission interrupted"));
            break;
        }

        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));

        // make sure we don't read too much from the mock data.
        if (sentLength + chunkSize > size) {
            chunkSize = size - sentLength;
        }

        char* chunkData = new char[chunkSize];
        file.read(chunkData, chunkSize);
        UInt8* data = reinterpret_cast<UInt8*>(chunkData);
        FileChunk* fileChunk = FileChunk::data(data, chunkSize);
        delete[] chunkData;

        events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, fileChunk));

        sentLength += chunkSize;
        file.seekg (sentLength, std::ios::beg);

        if (sentLength == size) {
            break;
        }
    }

    // send last message
    FileChunk* end = FileChunk::end();

    events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, end));

    file.close();

    s_isChunkingFile = false;
}

void
StreamChunker::sendTransferFile(const char* filename, IEventQueue* events, void* eventTarget)
{
	TransferManifest manifest;
	std::string manifestText;
	if (!TransferManifest::createForFile(filename, manifest) || !manifest.serialize(manifestText)) {
		throw runtime_error("failed to create transfer manifest");
	}
	std::ifstream file(filename, std::ios::in | std::ios::binary);
	if (!file.is_open()) throw runtime_error("failed to open file");
	const std::uint64_t id = g_nextTransferId++;
	beginTransfer(id);
	Event manifestEvent(events->forFile().fileChunkSending(), eventTarget);
	manifestEvent.setDataObject(new TransferEvent(TransferEvent::kManifest, id, 0, 0, manifestText, true));
	events->addEvent(manifestEvent);
	if (!waitForTransferAcceptance(id)) {
		Event finishedEvent(events->forFile().fileChunkSending(), eventTarget);
		finishedEvent.setDataObject(new TransferEvent(TransferEvent::kFinished, id, 0, 0, "", false));
		events->addEvent(finishedEvent);
		finishTransfer(id);
		return;
	}

	std::uint64_t offset = 0;
	char buffer[g_chunkSize];
	while (file.good()) {
		file.read(buffer, sizeof(buffer));
		const std::streamsize count = file.gcount();
		if (count <= 0) break;
		Event chunkEvent(events->forFile().fileChunkSending(), eventTarget);
		chunkEvent.setDataObject(new TransferEvent(TransferEvent::kChunk, id, 0, offset,
			std::string(buffer, static_cast<std::size_t>(count)), true));
		events->addEvent(chunkEvent);
		offset += count;
	}
	if (file.bad()) {
		finishTransfer(id);
		throw runtime_error("failed reading file");
	}
	Event finishedEvent(events->forFile().fileChunkSending(), eventTarget);
	finishedEvent.setDataObject(new TransferEvent(TransferEvent::kFinished, id, 0, offset, "", true));
	events->addEvent(finishedEvent);
	finishTransfer(id);
}

void
StreamChunker::beginTransfer(std::uint64_t id)
{
	std::lock_guard<std::mutex> lock(g_transferMutex);
	g_transferAcceptances[id] = TransferAcceptance{false, false};
}

void
StreamChunker::acceptTransfer(std::uint64_t id, bool accepted)
{
	std::lock_guard<std::mutex> lock(g_transferMutex);
	std::map<std::uint64_t, TransferAcceptance>::iterator transfer = g_transferAcceptances.find(id);
	if (transfer == g_transferAcceptances.end()) return;
	transfer->second.ready = true;
	transfer->second.accepted = accepted;
	g_transferCondition.notify_all();
}

bool
StreamChunker::waitForTransferAcceptance(std::uint64_t id)
{
	std::unique_lock<std::mutex> lock(g_transferMutex);
	std::map<std::uint64_t, TransferAcceptance>::iterator transfer = g_transferAcceptances.find(id);
	if (transfer == g_transferAcceptances.end()) return false;
	if (!g_transferCondition.wait_for(lock, std::chrono::seconds(30), [id]() {
		return g_transferAcceptances[id].ready;
	})) return false;
	return g_transferAcceptances[id].accepted;
}

void
StreamChunker::finishTransfer(std::uint64_t id)
{
	std::lock_guard<std::mutex> lock(g_transferMutex);
	g_transferAcceptances.erase(id);
}

void
StreamChunker::sendClipboard(
                String& data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget)
{
    // send first message (data size)
    String dataSize = barrier::string::sizeTypeToString(size);
    ClipboardChunk* sizeMessage = ClipboardChunk::start(id, sequence, dataSize);

    events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, sizeMessage));

    // send clipboard chunk with a fixed size
    size_t sentLength = 0;
    size_t chunkSize = g_chunkSize;

    while (true) {
        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));

        // make sure we don't read too much from the mock data.
        if (sentLength + chunkSize > size) {
            chunkSize = size - sentLength;
        }

        String chunk(data.substr(sentLength, chunkSize).c_str(), chunkSize);
        ClipboardChunk* dataChunk = ClipboardChunk::data(id, sequence, chunk);

        events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, dataChunk));

        sentLength += chunkSize;
        if (sentLength == size) {
            break;
        }
    }

    // send last message
    ClipboardChunk* end = ClipboardChunk::end(id, sequence);

    events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, end));

    LOG((CLOG_DEBUG "sent clipboard size=%d", sentLength));
}

void
StreamChunker::interruptFile()
{
    if (s_isChunkingFile) {
        s_interruptFile = true;
        LOG((CLOG_INFO "previous dragged file has become invalid"));
    }
}
