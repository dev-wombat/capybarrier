/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TransferManifest.h"

#include <limits>

namespace {

bool
readUnsigned(const std::string& text, std::string::size_type& position, std::uint64_t& value)
{
	const std::string::size_type begin = position;
	value = 0;
	while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
		const std::uint64_t digit = static_cast<std::uint64_t>(text[position] - '0');
		if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
			return false;
		}
		value = value * 10 + digit;
		++position;
	}
	return position != begin;
}

bool
readField(const std::string& text, std::string::size_type& position, std::string& value)
{
	std::uint64_t length = 0;
	if (!readUnsigned(text, position, length) || position == text.size() || text[position++] != ':') {
		return false;
	}
	if (length > text.size() - position) {
		return false;
	}
	value.assign(text, position, static_cast<std::string::size_type>(length));
	position += static_cast<std::string::size_type>(length);
	return true;
}

bool
isSha256(const std::string& value)
{
	if (value.size() != 64) {
		return false;
	}
	for (std::string::size_type i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

} // namespace

bool
TransferManifest::isSafeRelativePath(const std::string& path)
{
	if (path.empty() || path[0] == '/' || path[0] == '\\') {
		return false;
	}

	std::string::size_type begin = 0;
	while (begin < path.size()) {
		const std::string::size_type end = path.find('/', begin);
		const std::string component = path.substr(begin, end - begin);
		if (component.empty() || component == "." || component == "..") {
			return false;
		}
		if (end == std::string::npos) {
			return true;
		}
		begin = end + 1;
	}

	return false;
}

bool
TransferManifest::parse(const std::string& text, TransferManifest& manifest)
{
	std::vector<Entry> entries;
	std::string::size_type lineBegin = 0;
	while (lineBegin < text.size()) {
		const std::string::size_type lineEnd = text.find('\n', lineBegin);
		if (lineEnd == std::string::npos) {
			return false;
		}
		const std::string line = text.substr(lineBegin, lineEnd - lineBegin);
		if (line.size() < 3 || (line[0] != 'F' && line[0] != 'D') || line[1] != ' ') {
			return false;
		}

		std::string::size_type position = 2;
		Entry entry;
		entry.isDirectory = line[0] == 'D';
		if (!readField(line, position, entry.path) || !isSafeRelativePath(entry.path) ||
			position == line.size() || line[position++] != ' ' ||
			!readUnsigned(line, position, entry.size) || position == line.size() || line[position++] != ' ' ||
			!readField(line, position, entry.sha256) || position != line.size()) {
			return false;
		}
		if ((entry.isDirectory && (entry.size != 0 || !entry.sha256.empty())) ||
			(!entry.isDirectory && !isSha256(entry.sha256))) {
			return false;
		}
		entries.push_back(entry);
		lineBegin = lineEnd + 1;
	}

	if (entries.empty()) {
		return false;
	}
	manifest.m_entries.swap(entries);
	return true;
}

const std::vector<TransferManifest::Entry>&
TransferManifest::entries() const
{
	return m_entries;
}
