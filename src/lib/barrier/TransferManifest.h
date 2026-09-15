/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#ifndef BARRIER_LIB_BARRIER_TRANSFERMANIFEST_H
#define BARRIER_LIB_BARRIER_TRANSFERMANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

class TransferManifest {
public:
	struct Entry {
		bool isDirectory;
		std::string path;
		std::uint64_t size;
		std::string sha256;
	};

	static bool isSafeRelativePath(const std::string& path);
	static bool parse(const std::string& text, TransferManifest& manifest);

	const std::vector<Entry>& entries() const;

private:
	std::vector<Entry> m_entries;
};

#endif // BARRIER_LIB_BARRIER_TRANSFERMANIFEST_H
