/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TransferManifest.h"

#include "io/filesystem.h"

#include <openssl/evp.h>

#include <fstream>
#include <limits>
#include <sstream>

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

bool
sha256File(const barrier::fs::path& path, std::string& value)
{
	std::ifstream file;
	barrier::open_utf8_path(file, path, std::ios::in | std::ios::binary);
	if (!file.is_open()) return false;
	EVP_MD_CTX* context = EVP_MD_CTX_create();
	if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
		if (context != NULL) EVP_MD_CTX_destroy(context);
		return false;
	}
	char buffer[4096];
	while (file.good()) {
		file.read(buffer, sizeof(buffer));
		if (file.gcount() > 0 && EVP_DigestUpdate(context, buffer, static_cast<std::size_t>(file.gcount())) != 1) {
			EVP_MD_CTX_destroy(context);
			return false;
		}
	}
	if (file.bad()) { EVP_MD_CTX_destroy(context); return false; }
	unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
	if (EVP_DigestFinal_ex(context, digest, &length) != 1) { EVP_MD_CTX_destroy(context); return false; }
	EVP_MD_CTX_destroy(context);
	static const char hex[] = "0123456789abcdef";
	value.clear(); value.reserve(length * 2);
	for (unsigned int i = 0; i < length; ++i) { value.push_back(hex[digest[i] >> 4]); value.push_back(hex[digest[i] & 0x0f]); }
	return true;
}

} // namespace

bool
TransferManifest::isSafeRelativePath(const std::string& path)
{
	if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos) {
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

bool
TransferManifest::createForFile(const std::string& path, TransferManifest& manifest)
{
	const barrier::fs::path file(path);
	if (!barrier::fs::is_regular_file(file)) return false;
	Entry entry;
	entry.isDirectory = false;
	entry.path = file.filename().string();
	entry.size = barrier::fs::file_size(file);
	if (!isSafeRelativePath(entry.path) || !sha256File(file, entry.sha256)) return false;
	manifest.m_entries.assign(1, entry);
	return true;
}

bool
TransferManifest::serialize(std::string& text) const
{
	if (m_entries.empty()) return false;
	std::ostringstream output;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		const Entry& entry = m_entries[i];
		if (!isSafeRelativePath(entry.path) ||
			(entry.isDirectory && (entry.size != 0 || !entry.sha256.empty())) ||
			(!entry.isDirectory && !isSha256(entry.sha256))) return false;
		output << (entry.isDirectory ? 'D' : 'F') << ' ' << entry.path.size() << ':' << entry.path
			<< ' ' << entry.size << ' ' << entry.sha256.size() << ':' << entry.sha256 << '\n';
	}
	text = output.str();
	return true;
}

const std::vector<TransferManifest::Entry>&
TransferManifest::entries() const
{
	return m_entries;
}
