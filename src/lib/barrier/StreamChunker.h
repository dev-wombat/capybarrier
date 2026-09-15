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

#pragma once

#include "barrier/clipboard_types.h"
#include "base/Event.h"
#include "base/String.h"

#include <cstdint>

class IEventQueue;
class Mutex;

class StreamChunker {
public:
    class TransferEvent : public EventData {
    public:
        enum Type { kManifest, kChunk, kFinished };
        TransferEvent(Type type, std::uint64_t id, UInt32 entry, std::uint64_t offset,
                      const std::string& data, bool success) :
            m_type(type), m_id(id), m_entry(entry), m_offset(offset), m_data(data), m_success(success) { }
        Type m_type;
        std::uint64_t m_id;
        UInt32 m_entry;
        std::uint64_t m_offset;
        std::string m_data;
        bool m_success;
    };

    static void sendFile(const char* filename, IEventQueue* events, void* eventTarget);
    static void sendTransferFile(const char* filename, IEventQueue* events, void* eventTarget);
    static void beginTransfer(std::uint64_t id);
    static void acceptTransfer(std::uint64_t id, bool accepted);
    static void resumeTransfer(std::uint64_t id, UInt32 entry, std::uint64_t offset);
    static bool waitForTransferAcceptance(std::uint64_t id);
    static std::uint64_t transferOffset(std::uint64_t id);
    static void finishTransfer(std::uint64_t id);
    static void            sendClipboard(
                            String& data,
                            size_t size,
                            ClipboardID id,
                            UInt32 sequence,
                            IEventQueue* events,
                            void* eventTarget);
    static void            interruptFile();

private:
    static bool            s_isChunkingFile;
    static bool            s_interruptFile;
    static Mutex*        s_interruptMutex;
};
