/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TransferSession.h"

#include "io/filesystem.h"

#include <openssl/evp.h>

#include <fstream>

namespace {

bool
sha256File(const barrier::fs::path& path, std::string& value)
{
	std::ifstream file;
	barrier::open_utf8_path(file, path, std::ios::in | std::ios::binary);
	if (!file.is_open()) {
		return false;
	}

	EVP_MD_CTX* context = EVP_MD_CTX_create();
	if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_destroy(context);
		return false;
	}

	char buffer[4096];
	while (file.good()) {
		file.read(buffer, sizeof(buffer));
		const std::streamsize count = file.gcount();
		if (count > 0 && EVP_DigestUpdate(context, buffer, static_cast<std::size_t>(count)) != 1) {
			EVP_MD_CTX_destroy(context);
			return false;
		}
	}
	if (file.bad()) {
		EVP_MD_CTX_destroy(context);
		return false;
	}

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int length = 0;
	if (EVP_DigestFinal_ex(context, digest, &length) != 1) {
		EVP_MD_CTX_destroy(context);
		return false;
	}
	EVP_MD_CTX_destroy(context);

	static const char hex[] = "0123456789abcdef";
	value.clear();
	value.reserve(length * 2);
	for (unsigned int i = 0; i < length; ++i) {
		value.push_back(hex[digest[i] >> 4]);
		value.push_back(hex[digest[i] & 0x0f]);
	}
	return true;
}

} // namespace

bool
TransferSession::accept(const TransferManifest& manifest, const std::string& root)
{
	m_root = root;
	m_entries = manifest.entries();
	m_offsets.assign(m_entries.size(), VerifiedOffset{0});

	if (m_entries.empty()) {
		return false;
	}

	barrier::fs::create_directories(m_root);
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		const barrier::fs::path path = barrier::fs::path(m_root) / m_entries[i].path;
		if (m_entries[i].isDirectory) {
			barrier::fs::create_directories(path);
		}
		else {
			barrier::fs::create_directories(path.parent_path());
		}
	}
	return true;
}

bool
TransferSession::resumes(const TransferManifest& manifest) const
{
	const std::vector<TransferManifest::Entry>& entries = manifest.entries();
	if (entries.size() != m_entries.size()) return false;
	for (std::size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].isDirectory != m_entries[i].isDirectory ||
			entries[i].path != m_entries[i].path ||
			entries[i].size != m_entries[i].size ||
			entries[i].sha256 != m_entries[i].sha256) return false;
	}
	return true;
}

bool
TransferSession::writeChunk(std::size_t entry, std::uint64_t offset, const std::string& data)
{
	if (entry >= m_entries.size() || m_entries[entry].isDirectory ||
		offset != m_offsets[entry].offset || data.size() > m_entries[entry].size - offset) {
		return false;
	}

	const barrier::fs::path path = barrier::fs::path(m_root) / m_entries[entry].path;
	std::fstream file;
	barrier::open_utf8_path(file, path, std::ios::in | std::ios::out | std::ios::binary);
	if (!file.is_open()) {
		std::ofstream create;
		barrier::open_utf8_path(create, path, std::ios::out | std::ios::binary);
		create.close();
		barrier::open_utf8_path(file, path, std::ios::in | std::ios::out | std::ios::binary);
	}
	if (!file.is_open()) {
		return false;
	}
	file.seekp(static_cast<std::streamoff>(offset));
	file.write(data.data(), static_cast<std::streamsize>(data.size()));
	file.close();
	if (!file) {
		return false;
	}

	m_offsets[entry].offset += data.size();
	return true;
}

bool
TransferSession::finalize()
{
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].isDirectory) {
			continue;
		}
		std::string digest;
		const barrier::fs::path path = barrier::fs::path(m_root) / m_entries[i].path;
		if (m_offsets[i].offset != m_entries[i].size || !sha256File(path, digest) ||
			digest != m_entries[i].sha256) {
			barrier::fs::remove_all(m_root);
			return false;
		}
	}
	return true;
}

void
TransferSession::cancel()
{
	barrier::fs::remove_all(m_root);
}

const std::vector<TransferSession::VerifiedOffset>&
TransferSession::verifiedOffsets() const
{
	return m_offsets;
}
