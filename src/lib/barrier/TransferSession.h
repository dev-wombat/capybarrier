/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#ifndef BARRIER_LIB_BARRIER_TRANSFERSESSION_H
#define BARRIER_LIB_BARRIER_TRANSFERSESSION_H

#include "TransferManifest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class TransferSession {
public:
	struct VerifiedOffset {
		std::uint64_t offset;
	};

	bool accept(const TransferManifest& manifest, const std::string& root);
	bool writeChunk(std::size_t entry, std::uint64_t offset, const std::string& data);
	bool finalize();
	void cancel();

	const std::vector<VerifiedOffset>& verifiedOffsets() const;

private:
	std::string m_root;
	std::vector<TransferManifest::Entry> m_entries;
	std::vector<VerifiedOffset> m_offsets;
};

#endif // BARRIER_LIB_BARRIER_TRANSFERSESSION_H
